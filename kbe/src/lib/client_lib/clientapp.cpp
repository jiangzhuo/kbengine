/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "clientapp.h"
#include "event.h"
#include "entity.h"
#include "config.h"
#include "common/kbeversion.h"
#include "helper/script_loglevel.h"
#include "network/channel.h"
#include "network/tcp_packet_sender.h"
#include "network/tcp_packet_receiver.h"
#include "thread/threadpool.h"
#include "entitydef/entity_mailbox.h"
#include "entitydef/entitydef.h"
#include "server/components.h"
#include "server/serverconfig.h"
#include "helper/profile.h"
#include "client_lib/client_interface.h"

#include "../../server/baseapp/baseapp_interface.h"
#include "../../server/loginapp/loginapp_interface.h"

namespace KBEngine{

COMPONENT_TYPE g_componentType = UNKNOWN_COMPONENT_TYPE;
COMPONENT_ID g_componentID = 0;
COMPONENT_ORDER g_componentGlobalOrder = 1;
COMPONENT_ORDER g_componentGroupOrder = 1;
int32 g_genuuid_sections = -1;

GAME_TIME g_kbetime = 0;

KBE_SINGLETON_INIT(ClientApp);
//-------------------------------------------------------------------------------------
ClientApp::ClientApp(Network::EventDispatcher& dispatcher, 
					 Network::NetworkInterface& ninterface, 
					 COMPONENT_TYPE componentType,
					 COMPONENT_ID componentID):
ClientObjectBase(ninterface, getScriptType()),
TimerHandler(),
Network::ChannelTimeOutHandler(),
scriptBaseTypes_(),
gameTimer_(),
componentType_(componentType),
componentID_(componentID),
dispatcher_(dispatcher),
networkInterface_(ninterface),
pTCPPacketSender_(NULL),
pTCPPacketReceiver_(NULL),
pBlowfishFilter_(NULL),
threadPool_(),
entryScript_(),
state_(C_STATE_INIT)
{
	networkInterface_.pExtensionData(this);
	networkInterface_.pChannelTimeOutHandler(this);
	networkInterface_.pChannelDeregisterHandler(this);

	// 初始化mailbox模块获取channel函数地址
	EntityMailbox::setFindChannelFunc(std::tr1::bind(&ClientApp::findChannelByMailbox, this, 
		std::tr1::placeholders::_1));

	KBEngine::Network::MessageHandlers::pMainMessageHandlers = &ClientInterface::messageHandlers;

	Components::getSingleton().initialize(&ninterface, CLIENT_TYPE, g_componentID);
}

//-------------------------------------------------------------------------------------
ClientApp::~ClientApp()
{
	EntityMailbox::resetCallHooks();
	SAFE_RELEASE(pBlowfishFilter_);
}

//-------------------------------------------------------------------------------------		
void ClientApp::reset(void)
{
	state_ = C_STATE_INIT;

	if(pServerChannel_ && pServerChannel_->pEndPoint())
	{
		pServerChannel_->stopSend();
		networkInterface().dispatcher().deregisterReadFileDescriptor(*pServerChannel_->pEndPoint());
		networkInterface().deregisterChannel(pServerChannel_);
	}

	pServerChannel_->pFilter(NULL);
	pServerChannel_->pPacketSender(NULL);

	SAFE_RELEASE(pTCPPacketSender_);
	SAFE_RELEASE(pTCPPacketReceiver_);
	SAFE_RELEASE(pBlowfishFilter_);

	ClientObjectBase::reset();
}

//-------------------------------------------------------------------------------------
int ClientApp::registerPyObjectToScript(const char* attrName, PyObject* pyObj)
{ 
	return getScript().registerToModule(attrName, pyObj); 
}

//-------------------------------------------------------------------------------------
int ClientApp::unregisterPyObjectToScript(const char* attrName)
{ 
	return getScript().unregisterToModule(attrName); 
}

//-------------------------------------------------------------------------------------	
bool ClientApp::initializeBegin()
{
	gameTimer_ = this->dispatcher().addTimer(1000000 / g_kbeConfig.gameUpdateHertz(), this,
							reinterpret_cast<void *>(TIMEOUT_GAME_TICK));

	ProfileVal::setWarningPeriod(stampsPerSecond() / g_kbeConfig.gameUpdateHertz());

	Network::g_extReceiveWindowBytesOverflow = 0;
	Network::g_intReceiveWindowBytesOverflow = 0;
	Network::g_intReceiveWindowMessagesOverflow = 0;
	Network::g_extReceiveWindowMessagesOverflow = 0;
	Network::g_receiveWindowMessagesOverflowCritical = 0;
	return true;
}

//-------------------------------------------------------------------------------------	
bool ClientApp::initializeEnd()
{
	// 所有脚本都加载完毕
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("onInit"), 
										const_cast<char*>("i"), 
										0);

	if(pyResult != NULL)
	{
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	if(g_kbeConfig.useLastAccountName())
	{
		EventData_LastAccountInfo eventdata;
		eventdata.name = g_kbeConfig.accountName();
		eventHandler_.fire(&eventdata);
	}

	return true;
}

//-------------------------------------------------------------------------------------		
bool ClientApp::initialize()
{
	if(!threadPool_.isInitialize())
		threadPool_.createThreadPool(1, 1, 4);
	
	if(!initializeBegin())
		return false;

	if(!installPyModules())
		return false;
	
	if(!installEntityDef())
		return false;

	if(!inInitialize())
		return false;

	return initializeEnd();
}


//-------------------------------------------------------------------------------------
bool ClientApp::installEntityDef()
{
	if(!EntityDef::installScript(getScript().getModule()))
		return false;

	// 初始化所有扩展模块
	// assets/scripts/
	if(!EntityDef::initialize(scriptBaseTypes_, g_componentType)){
		return false;
	}

	// 注册一些接口到kbengine
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	publish,			__py_getAppPublish,								METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	fireEvent,			__py_fireEvent,									METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	player,				__py_getPlayer,									METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getSpaceData,		__py_GetSpaceData,								METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	callback,			__py_callback,									METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	cancelCallback,		__py_cancelCallback,							METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getWatcher,			__py_getWatcher,								METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getWatcherDir,		__py_getWatcherDir,								METH_VARARGS,	0)
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	disconnect,			__py_disconnect,								METH_VARARGS,	0)
	return true;
}

//-------------------------------------------------------------------------------------
void ClientApp::registerScript(PyTypeObject* pto)
{
	scriptBaseTypes_.push_back(pto);
}

//-------------------------------------------------------------------------------------
bool ClientApp::uninstallPyScript()
{
	unregisterPyObjectToScript("entities");
	return uninstallPyModules() && EntityDef::uninstallScript();
}

//-------------------------------------------------------------------------------------
bool ClientApp::installPyModules()
{
	registerScript(client::Entity::getScriptType());
	onInstallPyModules();

	// 注册设置脚本输出类型
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	scriptLogType,	__py_setScriptLogType,	METH_VARARGS,	0)
	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_NORMAL", log4cxx::ScriptLevel::SCRIPT_INT))
	{
		ERROR_MSG( "ClientApp::installPyModules: Unable to set KBEngine.LOG_TYPE_NORMAL.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_INFO", log4cxx::ScriptLevel::SCRIPT_INFO))
	{
		ERROR_MSG( "ClientApp::installPyModules: Unable to set KBEngine.LOG_TYPE_INFO.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_ERR", log4cxx::ScriptLevel::SCRIPT_ERR))
	{
		ERROR_MSG( "ClientApp::installPyModules: Unable to set KBEngine.LOG_TYPE_ERR.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_DBG", log4cxx::ScriptLevel::SCRIPT_DBG))
	{
		ERROR_MSG( "ClientApp::installPyModules: Unable to set KBEngine.LOG_TYPE_DBG.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_WAR", log4cxx::ScriptLevel::SCRIPT_WAR))
	{
		ERROR_MSG( "ClientApp::installPyModules: Unable to set KBEngine.LOG_TYPE_WAR.\n");
	}

	registerPyObjectToScript("entities", pEntities_);

	// 安装入口模块
	PyObject *entryScriptFileName = PyUnicode_FromString(g_kbeConfig.entryScriptFile());
	if(entryScriptFileName != NULL)
	{
		entryScript_ = PyImport_Import(entryScriptFileName);
		SCRIPT_ERROR_CHECK();
		S_RELEASE(entryScriptFileName);

		if(entryScript_.get() == NULL)
		{
			return false;
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool ClientApp::uninstallPyModules()
{
	return true;
}

//-------------------------------------------------------------------------------------		
void ClientApp::finalise(void)
{
	// 结束通知脚本
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("onFinish"),
										const_cast<char*>(""));

	if(pyResult != NULL)
	{
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}

	if(pServerChannel_ && pServerChannel_->pEndPoint())
	{
		pServerChannel_->stopSend();
		networkInterface().dispatcher().deregisterReadFileDescriptor(*pServerChannel_->pEndPoint());
		networkInterface().deregisterChannel(pServerChannel_);
	}

	pServerChannel_->pPacketSender(NULL);
	SAFE_RELEASE(pTCPPacketSender_);
	SAFE_RELEASE(pTCPPacketReceiver_);
	
	gameTimer_.cancel();
	threadPool_.finalise();
	ClientObjectBase::finalise();
	Network::finalise();

	uninstallPyModules();
	uninstallPyScript();
}

//-------------------------------------------------------------------------------------		
double ClientApp::gameTimeInSeconds() const
{
	return double(g_kbetime) / 10;
}

//-------------------------------------------------------------------------------------
void ClientApp::handleTimeout(TimerHandle, void * arg)
{
	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_GAME_TICK:
		{
			handleGameTick();
		}
		default:
			break;
	}
}

//-------------------------------------------------------------------------------------
void ClientApp::onServerClosed()
{
	ClientObjectBase::onServerClosed();
	state_ = C_STATE_INIT;
}

//-------------------------------------------------------------------------------------
void ClientApp::handleGameTick()
{
	g_kbetime++;
	threadPool_.onMainThreadTick();
	
	networkInterface().processChannels(KBEngine::Network::MessageHandlers::pMainMessageHandlers);
	tickSend();

	switch(state_)
	{
		case C_STATE_INIT:
			state_ = C_STATE_PLAY;
			break;
		case C_STATE_INITLOGINAPP_CHANNEL:
			state_ = C_STATE_PLAY;
			break;
		case C_STATE_LOGIN:

			state_ = C_STATE_PLAY;

			if(!ClientObjectBase::login())
			{
				WARNING_MSG("ClientApp::handleGameTick: login is failed!\n");
				return;
			}

			break;
		case C_STATE_LOGIN_BASEAPP_CHANNEL:
			{
				state_ = C_STATE_PLAY;

				bool ret = updateChannel(false, "", "", "", 0);
				if(ret)
				{
					// 先握手然后等helloCB之后再进行登录
					Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
					(*pBundle).newMessage(BaseappInterface::hello);
					(*pBundle) << KBEVersion::versionString();
					(*pBundle) << KBEVersion::scriptVersionString();

					if(Network::g_channelExternalEncryptType == 1)
					{
						pBlowfishFilter_ = new Network::BlowfishFilter();
						(*pBundle).appendBlob(pBlowfishFilter_->key());
						pServerChannel_->pFilter(NULL);
					}
					else
					{
						std::string key = "";
						(*pBundle).appendBlob(key);
					}

					pServerChannel_->pEndPoint()->send(pBundle);
					Network::Bundle::ObjPool().reclaimObject(pBundle);
					// ret = ClientObjectBase::loginBaseapp();
				}
			}
			break;
		case C_STATE_LOGIN_BASEAPP:

			state_ = C_STATE_PLAY;

			if(!ClientObjectBase::loginBaseapp())
			{
				WARNING_MSG("ClientApp::handleGameTick: loginBaseapp is failed!\n");
				return;
			}

			break;
		case C_STATE_PLAY:
			break;
		default:
			KBE_ASSERT(false);
			break;
	};
}

//-------------------------------------------------------------------------------------		
bool ClientApp::run(void)
{
	dispatcher_.processUntilBreak();
	return true;
}

//-------------------------------------------------------------------------------------		
int ClientApp::processOnce(bool shouldIdle)
{
	return dispatcher_.processOnce(shouldIdle);
}

//-------------------------------------------------------------------------------------
void ClientApp::onTargetChanged()
{ 
	// 所有脚本都加载完毕
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("onTargetChanged"), 
										const_cast<char*>("i"), 
										targetID_);

	if(pyResult != NULL)
	{
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}
}

//-------------------------------------------------------------------------------------		
PyObject* ClientApp::__py_getAppPublish(PyObject* self, PyObject* args)
{
	return PyLong_FromLong(g_appPublish);
}

//-------------------------------------------------------------------------------------		
PyObject* ClientApp::__py_getPlayer(PyObject* self, PyObject* args)
{
	client::Entity* pEntity = ClientApp::getSingleton().pPlayer();
	if(pEntity)
	{
		Py_INCREF(pEntity);
		return pEntity;
	}

	S_Return;
}

//-------------------------------------------------------------------------------------		
PyObject* ClientApp::__py_fireEvent(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) < 1)
	{
		PyErr_Format(PyExc_AssertionError, "ClientApp::fireEvent: arg1(eventName) not found!\n");
		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* pyname = PyTuple_GetItem(args, 0);

	if(!pyname || !PyUnicode_Check(pyname))
	{
		PyErr_Format(PyExc_AssertionError, "ClientApp::fireEvent: arg1(eventName) not found!\n");
		PyErr_PrintEx(0);
		return NULL;
	}

	wchar_t* PyUnicode_AsWideCharStringRet0 = PyUnicode_AsWideCharString(pyname, NULL);
	char* name = strutil::wchar2char(PyUnicode_AsWideCharStringRet0);
	PyMem_Free(PyUnicode_AsWideCharStringRet0);

	EventData_Script eventdata;
	eventdata.name = name;
	free(name);

	if(PyTuple_Size(args) - 1 > 0)
	{
		PyObject* pyitem = PyTuple_GetItem(args, 1);

		PyUnicode_AsWideCharStringRet0 = PyUnicode_AsWideCharString(pyitem, NULL);
		if(PyUnicode_AsWideCharStringRet0 == NULL)
		{
			PyErr_Format(PyExc_AssertionError, "ClientApp::fireEvent(%s): arg2 not is str!\n", eventdata.name.c_str());
			PyErr_PrintEx(0);
			return NULL;
		}

		char* datas = strutil::wchar2char(PyUnicode_AsWideCharStringRet0);
		PyMem_Free(PyUnicode_AsWideCharStringRet0);
		eventdata.datas = datas;
		free(datas);
	}

	ClientApp::getSingleton().fireEvent(&eventdata);
	S_Return;
}

//-------------------------------------------------------------------------------------	
PyObject* ClientApp::__py_setScriptLogType(PyObject* self, PyObject* args)
{
	int argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args is error!");
		PyErr_PrintEx(0);
		return 0;
	}

	int type = -1;

	if(PyArg_ParseTuple(args, "i", &type) == -1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args is error!");
		PyErr_PrintEx(0);
	}

	DebugHelper::getSingleton().setScriptMsgType(type);
	S_Return;
}

//-------------------------------------------------------------------------------------	
void ClientApp::shutDown()
{
	INFO_MSG("ClientApp::shutDown: shutting down\n");
	dispatcher_.breakProcessing();
}

//-------------------------------------------------------------------------------------	
void ClientApp::onChannelDeregister(Network::Channel * pChannel)
{
}

//-------------------------------------------------------------------------------------	
void ClientApp::onChannelTimeOut(Network::Channel * pChannel)
{
	INFO_MSG(fmt::format("ClientApp::onChannelTimeOut: "
		"Channel {} timed out.\n", pChannel->c_str()));

	networkInterface_.deregisterChannel(pChannel);
	pChannel->destroy();
	Network::Channel::ObjPool().reclaimObject(pChannel);
}

//-------------------------------------------------------------------------------------	
bool ClientApp::updateChannel(bool loginapp, std::string accountName, std::string passwd, 
								   std::string ip, KBEngine::uint32 port)
{
	if(pServerChannel_->pEndPoint())
	{
		pServerChannel_->stopSend();
		networkInterface().dispatcher().deregisterReadFileDescriptor(*pServerChannel_->pEndPoint());
		networkInterface().deregisterChannel(pServerChannel_);
	}

	bool ret = loginapp ? (initLoginappChannel(accountName, passwd, ip, port) != NULL) : (initBaseappChannel() != NULL);
	if(ret)
	{
		if(pTCPPacketReceiver_)
			pTCPPacketReceiver_->pEndPoint(pServerChannel_->pEndPoint());
		else
			pTCPPacketReceiver_ = new Network::TCPPacketReceiver(*pServerChannel_->pEndPoint(), networkInterface());

		if(pTCPPacketSender_)
			pTCPPacketSender_->pEndPoint(pServerChannel_->pEndPoint());
		else
			pTCPPacketSender_ = new Network::TCPPacketSender(*pServerChannel_->pEndPoint(), networkInterface());

		pServerChannel_->pPacketSender(pTCPPacketSender_);
		networkInterface().registerChannel(pServerChannel_);
		networkInterface().dispatcher().registerReadFileDescriptor(*pServerChannel_->pEndPoint(), pTCPPacketReceiver_);
	}

	return ret;
}

//-------------------------------------------------------------------------------------	
bool ClientApp::createAccount(std::string accountName, std::string passwd, std::string datas,
								   std::string ip, KBEngine::uint32 port)
{
	connectedBaseapp_ = false;

	if(canReset_)
		reset();

	clientDatas_ = datas;

	bool ret = updateChannel(true, accountName, passwd, ip, port);
	if(ret)
	{
		ret = ClientObjectBase::createAccount();
	}

	return ret;
}

//-------------------------------------------------------------------------------------	
bool ClientApp::login(std::string accountName, std::string passwd, std::string datas,
								   std::string ip, KBEngine::uint32 port)
{
	connectedBaseapp_ = false;

	if(canReset_)
		reset();

	clientDatas_ = datas;

	bool ret = updateChannel(true, accountName, passwd, ip, port);
	if(ret)
	{
		// 先握手然后等helloCB之后再进行登录
		Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
		(*pBundle).newMessage(LoginappInterface::hello);
		(*pBundle) << KBEVersion::versionString();
		(*pBundle) << KBEVersion::scriptVersionString();

		if(Network::g_channelExternalEncryptType == 1)
		{
			pBlowfishFilter_ = new Network::BlowfishFilter();
			(*pBundle).appendBlob(pBlowfishFilter_->key());
		}
		else
		{
			std::string key = "";
			(*pBundle).appendBlob(key);
		}

		pServerChannel_->pEndPoint()->send(pBundle);
		Network::Bundle::ObjPool().reclaimObject(pBundle);
		//ret = ClientObjectBase::login();
	}

	return ret;
}

//-------------------------------------------------------------------------------------	
void ClientApp::onHelloCB_(Network::Channel* pChannel, const std::string& verInfo, 
		const std::string& scriptVerInfo, const std::string& protocolMD5, const std::string& entityDefMD5, 
		COMPONENT_TYPE componentType)
{
	if(Network::g_channelExternalEncryptType == 1)
	{
		pServerChannel_->pFilter(pBlowfishFilter_);
		pBlowfishFilter_ = NULL;
	}

	if(componentType == LOGINAPP_TYPE)
	{
		state_ = C_STATE_LOGIN;
	}
	else
	{
		state_ = C_STATE_LOGIN_BASEAPP;
	}
}

//-------------------------------------------------------------------------------------	
void ClientApp::onVersionNotMatch(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObjectBase::onVersionNotMatch(pChannel, s);
}

//-------------------------------------------------------------------------------------	
void ClientApp::onScriptVersionNotMatch(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObjectBase::onScriptVersionNotMatch(pChannel, s);
}

//-------------------------------------------------------------------------------------	
void ClientApp::onLoginSuccessfully(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObjectBase::onLoginSuccessfully(pChannel, s);
	Config::getSingleton().writeAccountName(name_.c_str());

	state_ = C_STATE_LOGIN_BASEAPP_CHANNEL;
}

//-------------------------------------------------------------------------------------	
void ClientApp::onLoginFailed(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObjectBase::onLoginFailed(pChannel, s);
	canReset_ = true;
}

//-------------------------------------------------------------------------------------	
void ClientApp::onLoginBaseappFailed(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode)
{
	ClientObjectBase::onLoginBaseappFailed(pChannel, failedcode);
	canReset_ = true;
}

//-------------------------------------------------------------------------------------	
void ClientApp::onReLoginBaseappFailed(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode)
{
	ClientObjectBase::onReLoginBaseappFailed(pChannel, failedcode);
	canReset_ = true;
}

//-------------------------------------------------------------------------------------	
void ClientApp::onReLoginBaseappSuccessfully(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObjectBase::onReLoginBaseappSuccessfully(pChannel, s);
}

//-------------------------------------------------------------------------------------	
void ClientApp::onAddSpaceGeometryMapping(SPACE_ID spaceID, std::string& respath)
{
}

//-------------------------------------------------------------------------------------		
}
