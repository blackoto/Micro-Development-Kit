
#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#define strnicmp strncasecmp
#endif

#include <string>
#include <vector>
#include <list>
#include <iostream>
#include <assert.h>
#include <time.h>

#include "../../../include/mdk/mapi.h"
#include "../../../include/mdk/Socket.h"

#include "../../../include/frame/netserver/NetEngine.h"
#include "../../../include/frame/netserver/NetConnect.h"
#include "../../../include/frame/netserver/NetEventMonitor.h"
#include "../../../include/frame/netserver/NetServer.h"
#include "../../../include/mdk/atom.h"
#include "../../../include/mdk/MemoryPool.h"

using namespace std;
namespace mdk
{

NetEngine::NetEngine()
{
	m_stop = true;//停止标志
	m_startError = "";
	m_nHeartTime = 0;//心跳间隔(S)，默认不检查
	m_nReconnectTime = 0;//默认不自动重连
	m_pNetMonitor = NULL;
	m_ioThreadCount = 16;//网络io线程数量
	m_workThreadCount = 16;//工作线程数量
	m_pNetServer = NULL;
	m_averageConnectCount = 5000;
}

NetEngine::~NetEngine()
{
	Stop();
}

//设置平均连接数
void NetEngine::SetAverageConnectCount(int count)
{
	m_averageConnectCount = count;
}

//设置心跳时间
void NetEngine::SetHeartTime( int nSecond )
{
	m_nHeartTime = nSecond;
}

//设置自动重连时间,小于等于0表示不自动重连
void NetEngine::SetReconnectTime( int nSecond )
{
	m_nReconnectTime = nSecond;
}

//设置网络IO线程数量
void NetEngine::SetIOThreadCount(int nCount)
{
	m_ioThreadCount = nCount;//网络io线程数量
}

//设置工作线程数
void NetEngine::SetWorkThreadCount(int nCount)
{
	m_workThreadCount = nCount;//工作线程数量
}

/**
 * 开始引擎
 * 成功返回true，失败返回false
 */
bool NetEngine::Start()
{
	if ( !m_stop ) return true;
	m_stop = false;	
	m_pConnectPool = new MemoryPool( sizeof(NetConnect), m_averageConnectCount * 2 );
	Socket::SocketInit();
	if ( !m_pNetMonitor->Start( MAXPOLLSIZE ) ) 
	{
		m_startError = m_pNetMonitor->GetInitError();
		Stop();
		return false;
	}
	m_workThreads.Start( m_workThreadCount );
	int i = 0;
	for ( ; i < m_ioThreadCount; i++ ) m_ioThreads.Accept( Executor::Bind(&NetEngine::NetMonitorTask), this, NULL );
	m_ioThreads.Start( m_ioThreadCount );
	if ( !ListenAll() )
	{
		Stop();
		return false;
	}
	ConnectAll();
	return m_mainThread.Run( Executor::Bind(&NetEngine::Main), this, 0 );
}

void* NetEngine::NetMonitorTask( void* pParam)
{
	return NetMonitor( pParam );
}

//等待停止
void NetEngine::WaitStop()
{
	m_mainThread.WaitStop();
}

//停止引擎
void NetEngine::Stop()
{
	if ( m_stop ) return;
	m_stop = true;
	m_pNetMonitor->Stop();
	m_mainThread.Stop( 3 );
	m_ioThreads.Stop();
	m_workThreads.Stop();
	Socket::SocketDestory();
	if ( NULL != m_pConnectPool )
	{
		delete m_pConnectPool;
		m_pConnectPool = NULL;
	}
}

//主线程
void* NetEngine::Main(void*)
{
	time_t lastConnect = time(NULL);
	time_t curTime = time(NULL);
	while ( !m_stop ) 
	{
		curTime = time(NULL);
		HeartMonitor();
		if ( 0 < m_nReconnectTime && m_nReconnectTime <= curTime - lastConnect )
		{
			lastConnect = curTime;
			ConnectAll();
		}
		m_sleep( 10000 );
	}
	return NULL;
}

//心跳线程
void NetEngine::HeartMonitor()
{
	if ( m_nHeartTime <= 0 ) return;//无心跳机制
	//////////////////////////////////////////////////////////////////////////
	//关闭无心跳的连接
	ConnectList::iterator it;
	NetConnect *pConnect;
	time_t tCurTime = 0;
	tCurTime = time( NULL );
	time_t tLastHeart;
	AutoLock lock( &m_connectsMutex );
	for ( it = m_connectList.begin();  it != m_connectList.end(); )
	{
		pConnect = it->second;
		if ( pConnect->m_host.IsServer() ) //服务连接，不检查心跳
		{
			it++;
			continue;
		}
		//检查心跳
		tLastHeart = pConnect->GetLastHeart();
		if ( tCurTime < tLastHeart || tCurTime - tLastHeart < m_nHeartTime )//有心跳
		{
			it++;
			continue;
		}
		//无心跳/连接已断开，强制断开连接
		CloseConnect( it );
		it = m_connectList.begin();
	}
	lock.Unlock();
}

//关闭一个连接
void NetEngine::CloseConnect( ConnectList::iterator it )
{
	/*
	   必须先删除再关闭，顺序不能换，
	   避免关闭后，erase前，正好有client连接进来，
	   系统立刻就把该连接分配给新client使用，造成新client在插入m_connectList时失败
	*/
	NetConnect *pConnect = it->second;
	m_connectList.erase( it );//之后不可能有MsgWorker()发生，因为OnData里面已经找不到连接了
	pConnect->GetSocket()->Close();
	pConnect->m_bConnect = false;
	/*
		执行业务NetServer::OnClose();
		避免与未完成MsgWorker并发，(MsgWorker内部循环调用OnMsg())，也就是避免与OnMsg并发

		与MsgWorker的并发情况分析
		情况1：MsgWorker已经return
			那么AtomAdd返回0，执行NotifyOnClose()，不可能发生在OnMsg之前，
			之后也不可能OnMsg，前面已经说明MsgWorker()不可能再发生
		情况2：MsgWorker未返回，分2种情况
			情况1：这里先AtomAdd
				必然返回非0，因为没有发生过AtomDec
				不执行OnClose
				遗漏OnClose？
				不会！那么看MsgWorker()，AtomAdd返回非0，所以AtomDec必然返回>1，
				MsgWorker()会再循环一次OnMsg（这次OnMsg是没有数据的，对用户没有影响
				OnMsg读不到足够数据很正常），
				然后退出循环，发现m_bConnect=false，于是NotifyOnClose()发出OnClose通知
				OnClose通知没有被遗漏
			情况2：MsgWorker先AtomDec
				必然返回1，因为MsgWorker循环中首先置了1，而中间又没有AtomAdd发生
				MsgWorker退出循环
				发现m_bConnect=false，于是NotifyOnClose()发出OnClose通知
				然后这里AtomAdd必然返回0，也NotifyOnClose()发出OnClose通知
				重复通知？
				不会，NotifyOnClose()保证了多线程并发调用下，只会通知1次

		与OnData的并发情况分析
			情况1：OnData先AtomAdd
				保证有MsgWorker会执行
				AtomAdd返回非0，放弃NotifyOnClose
				MsgWorker一定会NotifyOnClose
			情况2：这里先AtomAdd
				OnData再AtomAdd时必然返回>0，OnData放弃MsgWorker
				遗漏OnMsg？应该算做放弃数据，而不是遗漏
				分3种断开情况
				1.server发现心跳没有了，主动close，那就是网络原因，强制断开，无所谓数据丢失
				2.client与server完成了所有业务，希望正常断开
					那就应该按照通信行业连接安全断开的原则，让接收方主动Close
					而不能发送方主动Close,所以不可能遗漏数据
					如果发送放主动close，服务器无论如何设计，都没办法保证收到最后的这次数据
	 */
	if ( 0 == AtomAdd(&pConnect->m_nReadCount, 1) ) NotifyOnClose(pConnect);
	pConnect->Release();//连接断开释放共享对象
	return;
}

void NetEngine::NotifyOnClose(NetConnect *pConnect)
{
	if ( 0 == AtomAdd(&pConnect->m_nDoCloseWorkCount, 1) )//只有1个线程执行OnClose，且仅执行1次
	{
		AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
		m_workThreads.Accept( Executor::Bind(&NetEngine::CloseWorker), this, pConnect);
	}
}

bool NetEngine::OnConnect( SOCKET sock, bool isConnectServer )
{
	NetConnect *pConnect = new (m_pConnectPool->Alloc())NetConnect(sock, isConnectServer, m_pNetMonitor, this, m_pConnectPool);
	if ( NULL == pConnect ) 
	{
		closesocket(sock);
		return false;
	}
	//加入管理列表
	AutoLock lock( &m_connectsMutex );
	pConnect->RefreshHeart();
	pair<ConnectList::iterator, bool> ret = m_connectList.insert( ConnectList::value_type(pConnect->GetSocket()->GetSocket(),pConnect) );
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();
	//执行业务
	m_workThreads.Accept( Executor::Bind(&NetEngine::ConnectWorker), this, pConnect );
	return true;
}

void* NetEngine::ConnectWorker( NetConnect *pConnect )
{
	m_pNetServer->OnConnect( pConnect->m_host );
	pConnect->Release();//使用完毕释放共享对象
	//监听连接
	/*
		 必须等OnConnect完成，才可以开始监听连接上的IO事件
		 否则，可能业务层尚未完成连接初始化工作，就收到OnMsg通知，
		 导致业务层不知道该如何处理消息
	 */
	if ( !MonitorConnect(pConnect) )
	{
		CloseConnect(pConnect->GetSocket()->GetSocket());
	}
	return 0;
}

void NetEngine::OnClose( SOCKET sock )
{
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(sock);
	if ( itNetConnect == m_connectList.end() )return;//底层已经主动断开
	CloseConnect( itNetConnect );
	lock.Unlock();
}

void* NetEngine::CloseWorker( NetConnect *pConnect )
{
	SetServerClose(pConnect);//连接的服务断开
	m_pNetServer->OnCloseConnect( pConnect->m_host );
	pConnect->Release();//使用完毕释放共享对象
	return 0;
}

connectState NetEngine::OnData( SOCKET sock, char *pData, unsigned short uSize )
{
	connectState cs = unconnect;
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(sock);//client列表里查找
	if ( itNetConnect == m_connectList.end() ) return cs;//底层已经断开

	NetConnect *pConnect = itNetConnect->second;
	pConnect->RefreshHeart();
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();//确保业务层占有对象后，HeartMonitor()才有机会检查pConnect的状态
	try
	{
		cs = RecvData( pConnect, pData, uSize );//派生类实现
		if ( unconnect == cs )
		{
			pConnect->Release();//使用完毕释放共享对象
			OnClose( sock );
			return cs;
		}
		/*
			避免并发MsgWorker，也就是避免并发读

			与MsgWorker的并发情况分析
			情况1：MsgWorker已经return
				那么AtomAdd返回0，触发新的MsgWorker，未并发

			情况2：MsgWorker未完成，分2种情况
				情况1：这里先AtomAdd
				必然返回非0，因为没有发生过AtomDec
				放弃触发MsgWorker
				遗漏OnMsg？
				不会！那么看MsgWorker()，AtomAdd返回非0，所以AtomDec必然返回>1，
				MsgWorker()会再循环一次OnMsg
				没有遗漏OnMsg，无并发
			情况2：MsgWorker先AtomDec
				必然返回1，因为MsgWorker循环中首先置了1，而中间又没有AtomAdd发生
				MsgWorker退出循环
				然后这里AtomAdd，必然返回0，触发新的MsgWorker，未并发
		 */
		if ( 0 < AtomAdd(&pConnect->m_nReadCount, 1) ) 
		{
			pConnect->Release();//使用完毕释放共享对象
			return cs;
		}
		//执行业务NetServer::OnMsg();
		m_workThreads.Accept( Executor::Bind(&NetEngine::MsgWorker), this, pConnect);
	}catch( ... ){}
	return cs;
}

void* NetEngine::MsgWorker( NetConnect *pConnect )
{
	for ( ; !m_stop; )
	{
		pConnect->m_nReadCount = 1;
		m_pNetServer->OnMsg( pConnect->m_host );//无返回值，避免框架逻辑依赖于客户实现
		if ( !pConnect->m_bConnect ) break;
		if ( pConnect->IsReadAble() ) continue;
		if ( 1 == AtomDec(&pConnect->m_nReadCount,1) ) break;//避免漏接收
	}
	//触发OnClose(),确保NetServer::OnClose()一定在所有NetServer::OnMsg()完成之后
	if ( !pConnect->m_bConnect ) NotifyOnClose(pConnect);
	pConnect->Release();//使用完毕释放共享对象
	return 0;
}

connectState NetEngine::RecvData( NetConnect *pConnect, char *pData, unsigned short uSize )
{
	return unconnect;
}

//关闭一个连接
void NetEngine::CloseConnect( SOCKET sock )
{
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find( sock );
	if ( itNetConnect == m_connectList.end() ) return;//底层已经主动断开
	CloseConnect( itNetConnect );
}

//响应发送完成事件
connectState NetEngine::OnSend( SOCKET sock, unsigned short uSize )
{
	connectState cs = unconnect;
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(sock);
	if ( itNetConnect == m_connectList.end() )return cs;//底层已经主动断开
	NetConnect *pConnect = itNetConnect->second;
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();//确保业务层占有对象后，HeartMonitor()才有机会检查pConnect的状态
	try
	{
		if ( pConnect->m_bConnect ) cs = SendData(pConnect, uSize);
	}
	catch(...)
	{
	}
	pConnect->Release();//使用完毕释放共享对象
	return cs;
	
}

connectState NetEngine::SendData(NetConnect *pConnect, unsigned short uSize)
{
	return unconnect;
}

bool NetEngine::Listen(int port)
{
	AutoLock lock(&m_listenMutex);
	pair<map<int,SOCKET>::iterator,bool> ret 
		= m_serverPorts.insert(map<int,SOCKET>::value_type(port,INVALID_SOCKET));
	map<int,SOCKET>::iterator it = ret.first;
	if ( !ret.second && INVALID_SOCKET != it->second ) return true;
	if ( m_stop ) return true;

	it->second = ListenPort(port);
	if ( INVALID_SOCKET == it->second ) return false;
	return true;
}

SOCKET NetEngine::ListenPort(int port)
{
	return INVALID_SOCKET;
}

bool NetEngine::ListenAll()
{
	bool ret = true;
	AutoLock lock(&m_listenMutex);
	map<int,SOCKET>::iterator it = m_serverPorts.begin();
	char strPort[256];
	string strFaild;
	for ( ; it != m_serverPorts.end(); it++ )
	{
		if ( INVALID_SOCKET != it->second ) continue;
		it->second = ListenPort(it->first);
		if ( INVALID_SOCKET == it->second ) 
		{
			sprintf( strPort, "%d", it->first );
			strFaild += strPort;
			strFaild += " ";
			ret = false;
		}
	}
	if ( !ret ) m_startError += "listen port:" + strFaild + "faild";
	return ret;
}

bool NetEngine::Connect(const char* ip, int port)
{
	uint64 addr64 = 0;
	if ( !addrToI64(addr64, ip, port) ) return false;

	AutoLock lock(&m_serListMutex);
	pair<map<uint64,SOCKET>::iterator,bool> ret 
		= m_serIPList.insert(map<uint64,SOCKET>::value_type(addr64,INVALID_SOCKET));
	map<uint64,SOCKET>::iterator it = ret.first;
	if ( !ret.second && INVALID_SOCKET != it->second ) return true;
	if ( m_stop ) return true;

	it->second = ConnectOtherServer(ip, port);
	if ( INVALID_SOCKET == it->second ) return false;
	lock.Unlock();

	if ( !OnConnect(it->second, true) )	it->second = INVALID_SOCKET;
	
	return true;
}

SOCKET NetEngine::ConnectOtherServer(const char* ip, int port)
{
	Socket sock;//监听socket
	if ( !sock.Init( Socket::tcp ) ) return INVALID_SOCKET;
	if ( !sock.Connect(ip, port) ) 
	{
		sock.Close();
		return INVALID_SOCKET;
	}
	Socket::InitForIOCP(sock.GetSocket());
	
	return sock.Detach();
}

bool NetEngine::ConnectAll()
{
	if ( m_stop ) return false;
	bool ret = true;
	AutoLock lock(&m_serListMutex);
	map<uint64,SOCKET>::iterator it = m_serIPList.begin();
	char ip[24];
	int port;
	for ( ; it != m_serIPList.end(); it++ )
	{
		if ( INVALID_SOCKET != it->second ) continue;
		i64ToAddr(ip, port, it->first);
		it->second = ConnectOtherServer(ip, port);
		if ( INVALID_SOCKET == it->second ) 
		{
			ret = false;
			continue;
		}
		if ( !OnConnect(it->second, true) )	it->second = INVALID_SOCKET;
	}

	return ret;
}

void NetEngine::SetServerClose(NetConnect *pConnect)
{
	if ( !pConnect->m_host.IsServer() ) return;
	
	SOCKET sock = pConnect->GetSocket()->GetSocket();
	AutoLock lock(&m_serListMutex);
	map<uint64,SOCKET>::iterator it = m_serIPList.begin();
	for ( ; it != m_serIPList.end(); it++ )
	{
		if ( sock != it->second ) continue;
		it->second = INVALID_SOCKET;
		break;
	}
}

//监听连接
bool NetEngine::MonitorConnect(NetConnect *pConnect)
{
	return false;
}

//向某组连接广播消息(业务层接口)
void NetEngine::BroadcastMsg( int *recvGroupIDs, int recvCount, char *msg, int msgsize, int *filterGroupIDs, int filterCount )
{
	//////////////////////////////////////////////////////////////////////////
	//关闭无心跳的连接
	ConnectList::iterator it;
	NetConnect *pConnect;
	vector<NetConnect*> recverList;
	//加锁将所有广播接收连接复制到一个队列中
	AutoLock lock( &m_connectsMutex );
	for ( it = m_connectList.begin(); m_nHeartTime > 0 && it != m_connectList.end(); it++ )
	{
		pConnect = it->second;
		if ( !pConnect->IsInGroups(recvGroupIDs, recvCount) 
			|| pConnect->IsInGroups(filterGroupIDs, filterCount) ) continue;
		recverList.push_back(pConnect);
		AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	}
	lock.Unlock();
	
	//向队列中的连接开始广播
	vector<NetConnect*>::iterator itv = recverList.begin();
	for ( ; itv != recverList.end(); itv++ )
	{
		pConnect = *itv;
		if ( pConnect->m_bConnect ) pConnect->SendData((const unsigned char*)msg,msgsize);
		pConnect->Release();//使用完毕释放共享对象
	}
}

//向某主机发送消息(业务层接口)
void NetEngine::SendMsg( int hostID, char *msg, int msgsize )
{
	AutoLock lock( &m_connectsMutex );
	ConnectList::iterator itNetConnect = m_connectList.find(hostID);
	if ( itNetConnect == m_connectList.end() ) return;//底层已经主动断开
	NetConnect *pConnect = itNetConnect->second;
	AtomAdd(&pConnect->m_useCount, 1);//业务层先获取访问
	lock.Unlock();
	if ( pConnect->m_bConnect ) pConnect->SendData((const unsigned char*)msg,msgsize);
	pConnect->Release();//使用完毕释放共享对象

	return;
}

const char* NetEngine::GetInitError()//取得启动错误信息
{
	return m_startError.c_str();
}

}
// namespace mdk

