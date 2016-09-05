#if !defined(_WIN32)

#include	<Network.h>
#include	<Logger.h>

#include	<algorithm>
#include	<cstdlib>
#include	<cstring>
#include	<map>
#include	<thread>
#include	<vector>

#include	<arpa/inet.h>
#include	<fcntl.h>
#include	<netinet/in.h>
#include	<sys/epoll.h>
#include	<sys/socket.h>
#include	<sys/types.h>
#include	<unistd.h>

#define		SOCKET_BUFSIZE	2097152

using namespace std;

struct SocketConnection {
	int			nSocket;
	sockaddr_in	iAddr;
};

typedef map<uint64_t, SocketConnection> Connections;

class SocketContext {
public:
	SocketContext(ISocket * pOwner);
	virtual ~SocketContext();

	int		Connect(const string & sIP, int nPort);
	bool	IsConnected() { return _nSocket >= 0; }
	void	Close(ENet::Close emCode);
	bool	Send(const char * pData, size_t nSize);
	void	Breath();

private:
	ISocket *		_pOwner;
	char *			_pReceived;
	int				_nSocket;
};

SocketContext::SocketContext(ISocket * pOwner)
	: _pOwner(pOwner)
	, _pReceived(new char[SOCKET_BUFSIZE])
	, _nSocket(-1) {}

SocketContext::~SocketContext() {
	Close(ENet::Local);
	delete[] _pReceived;
}

int SocketContext::Connect(const string & sIP, int nPort) {
	if (_nSocket >= 0) return ENet::Running;
	if ((_nSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) return errno;

	int nFlags = fcntl(_nSocket, F_GETFL, 0);
	if (fcntl(_nSocket, F_SETFL, O_NONBLOCK | nFlags) < 0) {
		close(_nSocket);
		_nSocket = -1;
		return errno;
	}

	struct sockaddr_in iAddr;
	memset(&iAddr, 0, sizeof(iAddr));

	iAddr.sin_family	= AF_INET;
	iAddr.sin_port		= htons(nPort);

	if (0 >= inet_pton(AF_INET, sIP.c_str(), &iAddr.sin_addr)) {
		close(_nSocket);
		_nSocket = -1;
		return ENet::BadParam;
	}

	if (connect(_nSocket, (sockaddr *)&iAddr, sizeof(sockaddr)) < 0) {
		if (errno != EINPROGRESS) return errno;

		fd_set iSet;
		struct timeval iWait;

		iWait.tv_sec = 3;
		iWait.tv_usec = 0;

		FD_ZERO(&iSet);
		FD_SET(_nSocket, &iSet);

		if (select(_nSocket + 1, 0, &iSet, 0, &iWait) <= 0) {
			close(_nSocket);
			_nSocket = -1;
			return ENet::Timeout;
		}

		int nErr;
		socklen_t nLen;
		if (getsockopt(_nSocket, SOL_SOCKET, SO_ERROR, &nErr, &nLen) < 0 || nErr != 0) {
			close(_nSocket);
			_nSocket = -1;
			return ENet::Closed;
		}
	}

	_pOwner->OnConnected();
	return 0;
}

void SocketContext::Close(ENet::Close emCode) {
	if (_nSocket < 0) return;
	close(_nSocket);
	_nSocket = -1;
	_pOwner->OnClose(emCode);
}

bool SocketContext::Send(const char * pData, size_t nSize) {
	if (_nSocket < 0) return false;

	char *	pSend	= (char *)pData;
	int		nSend	= 0;
	int		nLeft	= (int)nSize;

	while (true) {
		nSend = (int)send(_nSocket, pSend, nLeft, MSG_DONTWAIT);
		if (nSend < 0) {
			if (errno == EAGAIN) {
				usleep(1000);
			} else {
				return false;
			}
		} else if (nSend < nLeft) {
			nLeft -= nSend;
			pSend += nSend;
		} else if (nSend == nLeft) {
			return true;
		} else {
			return nLeft == 0;
		}
	}
}

void SocketContext::Breath() {
	if (_nSocket < 0) return;

	memset(_pReceived, 0, SOCKET_BUFSIZE);
	int nReaded = 0;

	while (true) {
		int nRecv = (int)recv(_nSocket, _pReceived + nReaded, SOCKET_BUFSIZE - nReaded, MSG_DONTWAIT);
		if (nRecv < 0) {
			if (errno == EAGAIN) {
				break;
			} else {
				Close(ENet::BadData);
				break;
			}
		} else if (nRecv == 0) {
			Close(ENet::Remote);
			break;
		} else {
			nReaded += nRecv;
		}
	}

	if (nReaded > 0) _pOwner->OnReceive(_pReceived, nReaded);
}

class ServerSocketContext {
public:
	ServerSocketContext(IServerSocket * pOwner);
	virtual ~ServerSocketContext();

	int		Listen(const string & sIP, int nPort);
	bool	Send(uint64_t nConnId, const char * pData, size_t nSize);
	void	Broadcast(const char * pData, size_t nSize);
	void	Close(uint64_t nConnId, ENet::Close emCode);
	void	Shutdown();
	void	Breath();

	IServerSocket::RemoteInfo	GetClientInfo(uint64_t nConnId);

private:
	IServerSocket *		_pOwner;
	char *				_pReceived;
	Connections			_mConns;
	map<int, uint64_t>	_mSocket2ConnId;
	int					_nSocket;
	int					_nIO;
};

ServerSocketContext::ServerSocketContext(IServerSocket * pOwner)
	: _pOwner(pOwner)
	, _pReceived(new char[SOCKET_BUFSIZE])
	, _mConns()
	, _nSocket(-1)
	, _nIO(0) {}

ServerSocketContext::~ServerSocketContext() {
	Shutdown();
	delete[] _pReceived;
}

int ServerSocketContext::Listen(const string & sIP, int nPort) {
	if (_nSocket >= 0) return ENet::Running;
	if ((_nSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) return errno;

	int nFlags = fcntl(_nSocket, F_GETFL, 0);
	if (fcntl(_nSocket, F_SETFL, O_NONBLOCK | nFlags) < 0) {
		close(_nSocket);
		_nSocket = -1;
		return errno;
	}

	struct sockaddr_in iAddr;
	memset(&iAddr, 0, sizeof(iAddr));

	iAddr.sin_family = AF_INET;
	iAddr.sin_port = htons(nPort);

	if (0 >= inet_pton(AF_INET, sIP.c_str(), &iAddr.sin_addr)) {
		close(_nSocket);
		_nSocket = -1;
		return ENet::BadParam;
	}

	if (::bind(_nSocket, (sockaddr *)&iAddr, sizeof(sockaddr)) < 0 || ::listen(_nSocket, 512) < 0) {
		close(_nSocket);
		_nSocket = -1;
		return errno;
	}

	if ((_nIO = epoll_create(1)) < 0) {
		close(_nSocket);
		_nSocket = -1;
		return ENet::Epoll;
	}

	struct epoll_event iEv;
	iEv.events = EPOLLIN | EPOLLET;
	iEv.data.fd = _nSocket;

	if (epoll_ctl(_nIO, EPOLL_CTL_ADD, _nSocket, &iEv) < 0) {
		close(_nIO);
		close(_nSocket);
		_nSocket = -1;
		return ENet::Epoll;
	}

	return 0;
}

bool ServerSocketContext::Send(uint64_t nConnId, const char * pData, size_t nSize) {
	auto it = _mConns.find(nConnId);
	if (it == _mConns.end()) return false;

	int		nSocket	= it->second.nSocket;
	char *	pSend	= (char *)pData;
	int		nSend	= 0;
	int		nLeft	= (int)nSize;

	while (true) {
		nSend = (int)send(nSocket, pSend, nLeft, MSG_DONTWAIT);
		if (nSend < 0) {
			if (errno == EAGAIN) {
				usleep(1000);
			} else {
				return false;
			}
		} else if (nSend < nLeft) {
			nLeft -= nSend;
			pSend += nSend;
		} else if (nSend == nLeft) {
			return true;
		} else {
			return nLeft == 0;
		}
	}
}

void ServerSocketContext::Broadcast(const char * pData, size_t nSize) {
	for (auto & r : _mConns) {
		int		nSocket	= r.second.nSocket;
		char *	pSend	= (char *)pData;
		int		nSend	= 0;
		int		nLeft	= (int)nSize;

		while (true) {
			nSend = (int)send(nSocket, pSend, nLeft, MSG_DONTWAIT);
			if (nSend < 0) {
				if (errno == EAGAIN) {
					usleep(1000);
				} else {
					break;
				}
			} else if (nSend < nLeft) {
				nLeft -= nSend;
				pSend += nSend;
			} else {
				break;
			}
		}
	}
}

void ServerSocketContext::Close(uint64_t nConnId, ENet::Close emCode) {
	auto it = _mConns.find(nConnId);
	if (it == _mConns.end()) return;

	_pOwner->OnClose(nConnId, emCode);

	epoll_ctl(_nIO, EPOLL_CTL_DEL, it->second.nSocket, NULL);
	close(it->second.nSocket);
	_mSocket2ConnId.erase(it->second.nSocket);
	_mConns.erase(it);
}

void ServerSocketContext::Shutdown() {
	if (_nSocket < 0) return;

	for (auto & r : _mConns) {
		_pOwner->OnClose(r.first, ENet::Local);
		epoll_ctl(_nIO, EPOLL_CTL_DEL, r.second.nSocket, NULL);
		close(r.second.nSocket);
	}

	epoll_ctl(_nIO, EPOLL_CTL_DEL, _nSocket, NULL);
	_mSocket2ConnId.clear();
	_mConns.clear();

	close(_nSocket);
	_nSocket = -1;
}

void ServerSocketContext::Breath() {
	if (_nSocket < 0) return;
	static epoll_event pEvents[512] = { 0 };
	static uint64_t nAllocId = 0;
	static sockaddr_in iAddr = { 0 };
	static socklen_t nSizeOfAddr = sizeof(iAddr);
	static char pAddr[128] = { 0 };

	int nCount = epoll_wait(_nIO, pEvents, 512, 0);
	if (nCount <= 0) return;

	for (int i = 0; i < nCount; ++i) {
		if (!(pEvents[i].events & EPOLLIN)) continue;

		if (pEvents[i].data.fd == _nSocket) {
			while (true) {
				int nAccept = accept(_nSocket, (sockaddr *)&iAddr, &nSizeOfAddr);
				if (nAccept < 0) break;

				uint64_t nConnId = nAllocId++;
				inet_ntop(AF_INET, &iAddr, pAddr, 128);

				struct epoll_event iEv;
				iEv.events = EPOLLIN | EPOLLET;
				iEv.data.fd = nAccept;

				if (epoll_ctl(_nIO, EPOLL_CTL_ADD, nAccept, &iEv) < 0) {
					LOG_WARN("Failed accept client [%s] while setting non-block!!!", pAddr);
					continue;
				}

				SocketConnection iInfo;
				iInfo.nSocket = nAccept;
				iInfo.iAddr = iAddr;

				IServerSocket::RemoteInfo iRemote;
				iRemote.nIP = iAddr.sin_addr.s_addr;
				iRemote.nPort = iAddr.sin_port;

				_mSocket2ConnId[nAccept] = nConnId;
				_mConns[nConnId] = iInfo;
				_pOwner->OnAccept(nConnId, iRemote);
			}
		} else {
			int nSocket = pEvents[i].data.fd;
			int nReaded = 0;

			auto it = _mSocket2ConnId.find(nSocket);
			if (it == _mSocket2ConnId.end()) continue;
			uint64_t nConnId = it->second;

			memset(_pReceived, 0, SOCKET_BUFSIZE);

			while (true) {
				int nRecv = (int)recv(nSocket, _pReceived + nReaded, SOCKET_BUFSIZE - nReaded, MSG_DONTWAIT);
				if (nRecv > 0) {
					nReaded += nRecv;
					if (nReaded >= SOCKET_BUFSIZE) {
						_pOwner->OnReceive(nConnId, _pReceived, nReaded);
						memset(_pReceived, 0, SOCKET_BUFSIZE);
						nReaded = 0;
					}
				} else if (nRecv < 0 && errno == EAGAIN) {
					if (nReaded > 0) _pOwner->OnReceive(nConnId, _pReceived, nReaded);
					break;
				} else {
					if (nReaded > 0) _pOwner->OnReceive(nConnId, _pReceived, nReaded);
					Close(nConnId, nRecv == 0 ? ENet::Remote : ENet::BadData);
					break;
				}
			}
		}
	}
}

IServerSocket::RemoteInfo ServerSocketContext::GetClientInfo(uint64_t nConnId) {
	IServerSocket::RemoteInfo iInfo;
	auto it = _mConns.find(nConnId);
	if (it == _mConns.end()) return move(iInfo);

	iInfo.nIP = it->second.iAddr.sin_addr.s_addr;
	iInfo.nPort = it->second.iAddr.sin_port;

	return move(iInfo);
}

class SocketGuard {
public:
	SocketGuard(ISocket * p, const string & sHost, int nPort);
	virtual ~SocketGuard();

	void	Start();

private:
	ISocket *	_p;
	string		_sHost;
	int			_nPort;
	thread *	_pWorker;
	bool		_bRunning;
};

SocketGuard::SocketGuard(ISocket * p, const string & sHost, int nPort)
	: _p(p), _sHost(sHost), _nPort(nPort), _pWorker(nullptr), _bRunning(false) {}

SocketGuard::~SocketGuard() {
	_bRunning = false;
	if (_pWorker) {
		if (_pWorker->joinable()) _pWorker->join();
		delete _pWorker;
	}
}

void SocketGuard::Start() {
	if (_bRunning) return;
	_bRunning = true;

	if (_pWorker) {
		if (_pWorker->joinable()) _pWorker->join();
		delete _pWorker;
	}

	_pWorker = new thread([this]() {
		while (_bRunning) {
			if (!_p->IsConnected()) {
				int n = _p->Connect(_sHost, _nPort, false);
				if (n != ENet::Success) LOG_WARN("Try to reconnect [%s:%d] ... %d", _sHost.c_str(), _nPort, n);
			}

			this_thread::sleep_for(chrono::seconds(1));
		}
	});
}

class NetworkBreather {
public:
	NetworkBreather() {}

	static NetworkBreather & Get();

	void	Add(ISocket * p);
	void	Add(IServerSocket * p);
	void	Del(ISocket * p);
	void	Del(IServerSocket * p);

	void	Breath();

private:
	vector<ISocket *>		_vClients;
	vector<IServerSocket *>	_vServers;
};

NetworkBreather & NetworkBreather::Get() {
	static unique_ptr<NetworkBreather> pIns;
	if (!pIns) pIns.reset(new NetworkBreather);
	return *pIns;
}

void NetworkBreather::Add(ISocket * p) {
	for (auto pClient : _vClients) {
		if (pClient == p) return;
	}

	_vClients.push_back(p);
}

void NetworkBreather::Add(IServerSocket * p) {
	for (auto pServer : _vServers) {
		if (pServer == p) return;
	}

	_vServers.push_back(p);
}

void NetworkBreather::Del(ISocket * p) {
	auto it = find(_vClients.begin(), _vClients.end(), p);
	if (it != _vClients.end()) _vClients.erase(it);
}

void NetworkBreather::Del(IServerSocket * p) {
	auto it = find(_vServers.begin(), _vServers.end(), p);
	if (it != _vServers.end()) _vServers.erase(it);
}

void NetworkBreather::Breath() {
	for (auto p : _vClients) p->Breath();
	for (auto p : _vServers) p->Breath();
}

void AutoNetworkBreath() {
	NetworkBreather::Get().Breath();
}

ISocket::ISocket() : _pCtx(nullptr), _pGuard(nullptr) {
	_pCtx = new SocketContext(this);
	NetworkBreather::Get().Add(this);
}

ISocket::~ISocket() {
	NetworkBreather::Get().Del(this);
	Close();
	if (_pGuard) delete _pGuard;
	if (_pCtx) delete _pCtx;
}

int ISocket::Connect(const std::string & sIP, int nPort, bool bAutoReconnect /* = false */) {
	if (sIP.empty() || nPort < 0) return ENet::BadParam;

	int n = _pCtx->Connect(sIP, nPort);
	if (n == 0 && bAutoReconnect && !_pGuard) {
		_pGuard = new SocketGuard(this, sIP, nPort);
		_pGuard->Start();
	}

	return n;
}

bool ISocket::IsConnected() {
	return _pCtx->IsConnected();
}

void ISocket::Close() {
	_pCtx->Close(ENet::Local);

	if (_pGuard) {
		delete _pGuard;
		_pGuard = nullptr;
	}
}

bool ISocket::Send(const char * pData, size_t nSize) {
	if (!pData || nSize <= 0) return false;
	return _pCtx->Send(pData, nSize);
}

void ISocket::Breath() {
	_pCtx->Breath();
}

IServerSocket::IServerSocket() : _pCtx(nullptr) {
	_pCtx = new ServerSocketContext(this);
	NetworkBreather::Get().Add(this);
}

IServerSocket::~IServerSocket() {
	NetworkBreather::Get().Del(this);
	Shutdown();
	if (_pCtx) delete _pCtx;
}

int IServerSocket::Listen(const std::string & sIP, int nPort) {
	if (sIP.empty() || nPort < 0) return ENet::BadParam;
	return _pCtx->Listen(sIP, nPort);
}

bool IServerSocket::Send(uint64_t nConnId, const char * pData, size_t nSize) {
	if (!pData || nSize <= 0) return false;
	return _pCtx->Send(nConnId, pData, nSize);
}

void IServerSocket::Broadcast(const char * pData, size_t nSize) {
	if (!pData || nSize <= 0) return;
	_pCtx->Broadcast(pData, nSize);
}

void IServerSocket::Close(uint64_t nConnId) {
	_pCtx->Close(nConnId, ENet::Local);
}

void IServerSocket::Shutdown() {
	_pCtx->Shutdown();
}

void IServerSocket::Breath() {
	_pCtx->Breath();
}

IServerSocket::RemoteInfo IServerSocket::GetClientInfo(uint64_t nConnId) {
	return _pCtx->GetClientInfo(nConnId);
}

string IServerSocket::RemoteInfo::GetIP() const {
	char pAddr[16];

	int n1 = (nIP & 0xFF);
	int n2 = (nIP >> 8) & 0xFF;
	int n3 = (nIP >> 16) & 0xFF;
	int n4 = (nIP >> 24) & 0xFF;

	snprintf(pAddr, 16, "%d.%d.%d.%d", n4, n3, n2, n1);
	return string(pAddr);
}

#endif