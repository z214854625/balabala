
#include "GatewayServer.h"

using namespace std;
using namespace bllsll;
// GatewayServer::GatewayServer()
// {
// }

// GatewayServer::~GatewayServer()
// {
// }

bool GatewayServer::Create()
{

}

void GatewayServer::Release()
{

}

GatewayServer* CreateGatewayServer()
{
	CEnterManager * pEnterManager = new CEnterManager();
	if (pEnterManager == nullptr || !pEnterManager->Create())
	{
		SAFE_RELEASE(pEnterManager);
		return nullptr;
	}
	
	return pEnterManager;
}