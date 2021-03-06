
#include "unistd.h"
#include "signal.h"
#include "errno.h"


#include <I2c.hpp>
#include "I2cDevice.hpp"
#include "RemoteAardvark.hpp"
#include "allocators.h"


I2c::I2c() : RPCInterface<I2c*, i2cfptr>(this)
{
	i2cfptr fptr;

	subResponse = NULL;
	error = NULL;
	subRequest = NULL;
	subResult = NULL;
	requestId = NULL;
	mainResponse = NULL;
	json = new JsonRPC();
	mainRequestDom = new Document();
	subResponseDom = new Document();

	//configure signal SIGUSR2 and timeout for receiving subresponses
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	timeout.tv_sec = SUBRESPONSE_TIMEOUT;
	timeout.tv_nsec = 0;


	fptr = &I2c::write;
	funcMap.insert(pair<const char*, i2cfptr>("i2c.write", fptr));
	fptr = &I2c::read;
	funcMap.insert(pair<const char*, i2cfptr>("i2c.read", fptr));
	fptr = &I2c::getAardvarkDevices;
	funcMap.insert(pair<const char*, i2cfptr>("i2c.getAardvarkDevices", fptr));
	fptr= &I2c::getI2cDevices;
	funcMap.insert(pair<const char*, i2cfptr>("i2c.getI2cDevices", fptr));
}


I2c::~I2c()
{
	delete json;
	delete mainRequestDom;
	delete subResponseDom;
	deleteDeviceList();
};


OutgoingMsg* I2c::process(IncomingMsg* input)
{
	Value result;
	Value* params = NULL;
	OutgoingMsg* output = NULL;
	Value* requestMethod = NULL;

	try
	{
		json->parse(mainRequestDom, input->getContent());
		if(json->isRequest(mainRequestDom))
		{
			setBusy(true);
			requestMethod = json->tryTogetMethod(mainRequestDom);
			params = json->tryTogetParams(mainRequestDom);
			requestId = json->getId(mainRequestDom);
			executeFunction(*requestMethod, *params, result);
			output = new OutgoingMsg(input->getOrigin(), mainResponse);
			setBusy(false);
		}
		else if(json->isNotification(mainRequestDom))
		{
			//do nothing;
		}
	}
	catch(Error &e)
	{
		error = json->generateResponseError(*requestId, e.getErrorCode(), e.get());
		output = new OutgoingMsg(input->getOrigin(), error);
		setBusy(false);
	}
	delete input;
	return output;
}


bool I2c::isSubResponse(RPCMsg* rpcMsg)
{
	bool result = false;
	Value tempId;

	try
	{
		json->parse(subResponseDom, rpcMsg->getContent());
		if(json->isResponse(subResponseDom))
		{
			tempId.CopyFrom(*(json->getId(subResponseDom)), subResponseDom->GetAllocator());
			if(tempId == *requestId)
				result = true;
			else
				result = false;
		}
	}
	catch(Error &e)
	{
		result = false;
	}
	return result;
}



bool I2c::getI2cDevices(Value &params, Value &result)
{

	getAardvarkDevices(params, result);
	//.. call further methods for getting other devices
	//generate jsonrpc rsponse like {..... "result" : {Aardvark : [id1, id2, idx] , OtherDevice : [id1, id2, idx]}}
	mainResponse = json->generateResponse(*requestId, result);

	return true;
}



bool I2c::getAardvarkDevices(Value &params, Value &result)
{
	//generate json rpc for aa_find_devices ext
	Value method;
	Value currentParam;
	Value* i2cDeviceValue = NULL;
	Value* i2cUniqueIdValue = NULL;
	int num_devices = 0;

	//get the DOM for generating requests
	Document* requestDom = json->getRequestDOM();

	//Generate subRequest
	method.SetString(_aa_find_devices_ext._name, requestDom->GetAllocator());
	params.SetObject();
	currentParam.SetString(_aa_find_devices_ext.paramArray[0]._name, requestDom->GetAllocator());
	params.AddMember( currentParam, 256, requestDom->GetAllocator());


	subRequest = json->generateRequest(method, params, *requestId);

	//Send subRequest and wait for subResponse
	comPoint->transmit(subRequest, strlen(subRequest));
    waitForResponse();

	subResult = json->tryTogetResult(subResponseDom);
	i2cDeviceValue = json->findObjectMember(*subResult, "devices");
	i2cUniqueIdValue = json->findObjectMember(*subResult, "unique_ids");
	num_devices = i2cDeviceValue->Size();


	currentParam.SetArray();
	for(int i = 0; i < num_devices; i++)
	{
		deviceList.push_back(new I2cDevice("Aardvark", (*i2cDeviceValue)[i].GetInt(), (*i2cUniqueIdValue)[i].GetUint()));
		currentParam.PushBack((*i2cUniqueIdValue)[i].GetUint(), requestDom->GetAllocator());
	}


	result.SetObject();
	result.AddMember("Aardvark", currentParam, requestDom->GetAllocator());
	return true;
}



bool I2c::write(Value &params, Value &result)
{

	try
	{
		rapidjson::MemoryPoolAllocator<> &subRequestAllocator = json->getRequestDOM()->GetAllocator();

		//call subMethods
		aa_open(params);
		//param powerMask will be always the same, but is not send by mainRequest.
		params.AddMember("powerMask", AA_TARGET_POWER_BOTH, subRequestAllocator);
		aa_target_power(params);
		aa_write(params);
		aa_close(params);

		//generate mainResponse
		result.SetObject();
		result.AddMember("returnCode", "OK", json->getResponseDOM()->GetAllocator());
		mainResponse = json->generateResponse(*requestId, result);
	}
	catch(Error &e)
	{
		throw;
	}
	return true;
}


bool I2c::read(Value &params, Value &result)
{
	try
	{
		Value data_out;
		rapidjson::MemoryPoolAllocator<> &subRequestAllocator = json->getRequestDOM()->GetAllocator();
		result.SetObject();

		aa_open(params);
		params.AddMember("powerMask", AA_TARGET_POWER_BOTH, subRequestAllocator);
		aa_target_power(params);

		data_out.SetArray();
		data_out.PushBack(params["mem_addr"], subRequestAllocator);
		params.AddMember("data_out", data_out, subRequestAllocator);

		params.AddMember("AardvarkI2cFlags", AA_I2C_NO_STOP, subRequestAllocator);
		aa_write(params);

		params.EraseMember("AardvarkI2cFlags");
		aa_read(params, result);
		aa_close(params);

		result.AddMember("returnCode", "OK", subRequestAllocator);
		mainResponse = json->generateResponse(*requestId, result);
	}
	catch(Error &e)
	{
		throw;
	}
	return true;
}




void I2c::aa_open(Value &params)
{
	Value method;
	Value localParams;
	Value tempParam;
	Value* subResultValue= NULL;
	Value* deviceValue = NULL;
	int device = 0;


	//Get exact method name
	method.SetString(_aa_open._name, subRequestAllocator);
	//Get all needed params
	localParams.SetObject();
	tempParam.SetString(_aa_open.paramArray[0]._name , subRequestAllocator);
	//map "device" -> to "Aardvark"(_aa_open.paramArray[0]._name)
	deviceValue = json->findObjectMember(params, "device");
	device = getPortByUniqueId(deviceValue->GetUint());
	localParams.AddMember(tempParam, device, subRequestAllocator);
	subRequest = json->generateRequest(method, localParams, *requestId);

	//send subRequest, wait for subresponse and parse subResponse to localDom (not overwriting dom of I2c)
	comPoint->transmit(subRequest, strlen(subRequest));
	waitForResponse();

	if(checkSubResult(subResponseDom))
	{
		subResult = json->tryTogetResult(subResponseDom);
		subResultValue = json->findObjectMember(*subResult, "Aardvark", kNumberType);

		if(subResultValue->GetInt() < 0)
			throw Error(subResponse);
		else
			params.AddMember("Aardvark", subResultValue->GetInt(), subRequestAllocator);
	}
	else
	{
		throw Error("todo");
	}
}


void I2c::aa_target_power(Value &params)
{
	Value method;
	Value localParams;
	Value tempParam;
	Value* subResultValue= NULL;

	localParams.SetObject();
	Value* valuePtr = json->findObjectMember(params, _aa_target_power.paramArray[0]._name);

	//Aardvark handle
	tempParam.SetString(_aa_target_power.paramArray[0]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);

	//powerMask
	valuePtr = json->findObjectMember(params, _aa_target_power.paramArray[1]._name);
	tempParam.SetString(_aa_target_power.paramArray[1]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);

	//methodname
	method.SetString(_aa_target_power._name, subRequestAllocator);

	subRequest = json->generateRequest(method, localParams, *requestId);

	comPoint->transmit(subRequest, strlen(subRequest));
	waitForResponse();


	subResult = json->tryTogetResult(subResponseDom);

	subResultValue = json->findObjectMember(*subResult, "returnCode", kNumberType);

	if(subResultValue->GetInt() < 0)
	{
		throw Error("Could not open Aardvark.");
	}
}


void I2c::aa_write(Value &params)
{
	Value method;
	Value localParams;
	Value tempParam;
	Value array;
	Value* valuePtr = NULL;
	Value* subResultValue= NULL;


	localParams.SetObject();
	//get Aardvark handle
	valuePtr = json->findObjectMember(params, _aa_i2c_write.paramArray[0]._name);
	tempParam.SetString(_aa_i2c_write.paramArray[0]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);

	//get slave addr
	valuePtr = json->findObjectMember(params, _aa_i2c_write.paramArray[1]._name);
	tempParam.SetString(_aa_i2c_write.paramArray[1]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);

	//get flags, flags are optional
	tempParam.SetString(_aa_i2c_write.paramArray[2]._name, subRequestAllocator);
	if(params.HasMember(_aa_i2c_write.paramArray[2]._name))
	{
		valuePtr = json->findObjectMember(params, _aa_i2c_write.paramArray[2]._name);
		localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);
	}
	else
		localParams.AddMember(tempParam, AA_I2C_NO_FLAGS, subRequestAllocator);

	//get data
	valuePtr = json->findObjectMember(params, _aa_i2c_write.paramArray[3]._name);
	tempParam.SetString(_aa_i2c_write.paramArray[3]._name, subRequestAllocator);
	localParams.AddMember(tempParam, *valuePtr, subRequestAllocator);

	method.SetString(_aa_i2c_write._name, subRequestAllocator);
	subRequest = json->generateRequest(method, localParams, *requestId);

	comPoint->transmit(subRequest, strlen(subRequest));
	waitForResponse();


	subResult = json->tryTogetResult(subResponseDom);
	subResultValue = json->findObjectMember(*subResult, "returnCode", kNumberType);

	if(subResultValue->GetInt() < 0)
		throw Error("Could not open Aardvark.");

}


void I2c::aa_read(Value &params, Value &result)
{
	Value method;
	Value localParams;
	Value tempParam;
	Value array;
	Value* valuePtr = NULL;
	Value* subResultValue= NULL;

	rapidjson::MemoryPoolAllocator<> &subRequestAllocator = json->getRequestDOM()->GetAllocator();


	localParams.SetObject();
	//get Aardvark handle
	valuePtr = json->findObjectMember(params, _aa_i2c_read.paramArray[0]._name);
	tempParam.SetString(_aa_i2c_read.paramArray[0]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);

	//get slave addr
	valuePtr = json->findObjectMember(params, _aa_i2c_read.paramArray[1]._name);
	tempParam.SetString(_aa_i2c_read.paramArray[1]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);


	//get flags, flags are optional
	tempParam.SetString(_aa_i2c_write.paramArray[2]._name, subRequestAllocator);
	if(params.HasMember(_aa_i2c_write.paramArray[2]._name))
	{
		valuePtr = json->findObjectMember(params, _aa_i2c_write.paramArray[2]._name);
		localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);
	}
	else
		localParams.AddMember(tempParam, AA_I2C_NO_FLAGS, subRequestAllocator);


	//num_bytes
	valuePtr = json->findObjectMember(params, _aa_i2c_read.paramArray[3]._name);
	tempParam.SetString(_aa_i2c_read.paramArray[3]._name, subRequestAllocator);
	localParams.AddMember(tempParam, valuePtr->GetInt(), subRequestAllocator);

	method.SetString(_aa_i2c_read._name, subRequestAllocator);
	subRequest = json->generateRequest(method, localParams, *requestId);

	comPoint->transmit(subRequest, strlen(subRequest));
	waitForResponse();


	subResult = json->tryTogetResult(subResponseDom);
	subResultValue = json->findObjectMember(*subResult, "returnCode", kNumberType);

	if(subResultValue->GetInt() < 0)
		throw Error("Could not open Aardvark.");

	//add member data_in from subresponse to result of mainresponse
	subResultValue = json->findObjectMember(*subResult, "data_in", kArrayType);
	result.AddMember("data_in", *subResultValue, subRequestAllocator);

}




void I2c::aa_close(Value &params)
{
	Value method;
	Value localParams;
	Value* subResultValue= NULL;

	Value* deviceValue = NULL;
	Value tempParam;

	//Get exact method name
	method.SetString(_aa_close._name, subRequestAllocator);
	//Get all needed params
	localParams.SetObject();
	tempParam.SetString(_aa_close.paramArray[0]._name , subRequestAllocator);

	deviceValue = json->findObjectMember(params, _aa_close.paramArray[0]._name);
	localParams.AddMember(tempParam, *deviceValue, subRequestAllocator);

	subRequest = json->generateRequest(method, localParams, *requestId);

	//send subRequest, wait for subresponse and parse subResponse to localDom (not overwriting dom of I2c)
	comPoint->transmit(subRequest, strlen(subRequest));
	waitForResponse();


	json->tryTogetResult(subResponseDom);

	subResultValue = json->findObjectMember(*subResult, "returnCode", kNumberType);


	if(subResultValue->GetInt() < 0)
	{
		throw Error("Could not close Aardvark.");
	}
	else
		params.AddMember("returnCode", subResultValue->GetInt(), subRequestAllocator);
}


int I2c::getPortByUniqueId(unsigned int uniqueId)
{
	list<I2cDevice*>::iterator device = deviceList.begin();
	unsigned int result = -1;

	while( device != deviceList.end())
	{
		result = (*device)->getIdentification();
		if(uniqueId == result)
		{
			return (*device)->getPort();
		}
		++device;
	}

	return result;
}


bool I2c::checkSubResult(Document* dom)
{
	bool result = false;

	if(json->isError(dom))
		result = false;
	else
		result = true;

	return result;

}


void I2c::waitForResponse()
{
	int retCode = 0;
	bool noTimeout = true;

	while(noTimeout)
	{
		retCode = sigtimedwait(&set, NULL, &timeout);
		if(retCode < 0)
		{
			printf("%s\n", strerror(errno));
			noTimeout = false;
			throw Error("Timeout waiting for subResponse.");
		}
		noTimeout = false;
	}
}

void I2c::deleteDeviceList()
{
	list<I2cDevice*>::iterator device = deviceList.begin();
	while( device != deviceList.end())
	{
		delete *device;
		device = deviceList.erase(device);
	}
}

