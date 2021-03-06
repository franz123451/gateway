#include <sstream>

#include <Poco/NumberParser.h>
#include <Poco/Util/AbstractConfiguration.h>

#include "commands/DeviceAcceptCommand.h"
#include "commands/DeviceSetValueCommand.h"
#include "commands/DeviceUnpairCommand.h"
#include "commands/GatewayListenCommand.h"
#include "commands/NewDeviceCommand.h"

#include "core/CommandDispatcher.h"
#include "core/Distributor.h"
#include "di/Injectable.h"
#include "model/SensorData.h"
#include "vdev/VirtualDeviceManager.h"

BEEEON_OBJECT_BEGIN(BeeeOn, VirtualDeviceManager)
BEEEON_OBJECT_CASTABLE(StoppableRunnable)
BEEEON_OBJECT_CASTABLE(CommandHandler)
BEEEON_OBJECT_CASTABLE(DeviceStatusHandler)
BEEEON_OBJECT_PROPERTY("deviceCache", &VirtualDeviceManager::setDeviceCache)
BEEEON_OBJECT_PROPERTY("file", &VirtualDeviceManager::setConfigFile)
BEEEON_OBJECT_PROPERTY("distributor", &VirtualDeviceManager::setDistributor)
BEEEON_OBJECT_PROPERTY("devicePoller", &VirtualDeviceManager::setDevicePoller)
BEEEON_OBJECT_PROPERTY("commandDispatcher", &VirtualDeviceManager::setCommandDispatcher)
BEEEON_OBJECT_HOOK("done", &VirtualDeviceManager::installVirtualDevices)
BEEEON_OBJECT_END(BeeeOn, VirtualDeviceManager)

using namespace BeeeOn;
using namespace Poco;
using namespace Poco::Util;
using namespace std;

const static unsigned int DEFAULT_REFRESH_SECS = 30;

VirtualDeviceManager::VirtualDeviceManager():
	DeviceManager(DevicePrefix::PREFIX_VIRTUAL_DEVICE, {
		typeid(GatewayListenCommand),
		typeid(DeviceAcceptCommand),
		typeid(DeviceUnpairCommand),
		typeid(DeviceSetValueCommand),
	})
{
}

void VirtualDeviceManager::setDevicePoller(DevicePoller::Ptr poller)
{
	m_pollingKeeper.setDevicePoller(poller);
}

void VirtualDeviceManager::registerDevice(
	const VirtualDevice::Ptr device)
{
	if (!m_virtualDevicesMap.emplace(device->id(), device).second) {
		throw ExistsException("registering duplicate device: "
			+ device->id().toString());
	}

	logger().debug(
		"registering new virtual device "
		+ device->id().toString(),
		__FILE__, __LINE__
	);
}

void VirtualDeviceManager::logDeviceParsed(VirtualDevice::Ptr device)
{
	logger().information(
		"virtual device: "
		+ device->id().toString(),
		__FILE__, __LINE__
	);

	logger().debug(
		"virtual device: "
		+ device->id().toString()
		+ ", modules: "
		+ to_string(device->modules().size())
		+ ", paired: "
		+ (deviceCache()->paired(device->id()) ? "yes" : "no")
		+ ", refresh: "
		+ device->refresh().toString()
		+ ", vendor: "
		+ device->vendorName()
		+ ", product: "
		+ device->productName(),
		__FILE__, __LINE__
	);

	for (auto &module : device->modules()) {
		logger().trace(
			"virtual device: "
			+ device->id().toString()
			+ ", module: "
			+ module->moduleID().toString()
			+ ", type: "
			+ module->moduleType().type().toString(),
			__FILE__, __LINE__
		);
	}
}

VirtualDevice::Ptr VirtualDeviceManager::parseDevice(
	AutoPtr <AbstractConfiguration> cfg)
{
	VirtualDevice::Ptr device = new VirtualDevice;

	DeviceID id = DeviceID::parse(cfg->getString("device_id"));
	if (id.prefix() != DevicePrefix::PREFIX_VIRTUAL_DEVICE) {
		device->setID(DeviceID(DevicePrefix::PREFIX_VIRTUAL_DEVICE, id.ident()));

		logger().warning(
			"device prefix was wrong, overriding ID to "
			+ device->id().toString(),
			__FILE__, __LINE__
		);
	}
	else {
		device->setID(id);
	}

	unsigned int refresh = cfg->getUInt("refresh", DEFAULT_REFRESH_SECS);
	device->setRefresh(RefreshTime::fromSeconds(refresh));

	if (cfg->getBool("paired", false))
		deviceCache()->markPaired(id);
	else
		deviceCache()->markUnpaired(id);

	device->setVendorName(cfg->getString("vendor"));
	device->setProductName(cfg->getString("product"));

	for (int i = 0; cfg->has("module" + to_string(i) + ".type"); ++i) {
		try {
			AutoPtr<AbstractConfiguration> view =
				cfg->createView("module" + to_string(i));
			device->addModule(parseModule(view, i));
		}
		catch (const Exception &ex) {
			logger().log(ex, __FILE__, __LINE__);
			logger().critical(
				"failed to initialize module "
				+ id.toString(),
				__FILE__, __LINE__
			);
			break;
		}
	}
	logDeviceParsed(device);

	return device;
}

VirtualModule::Ptr VirtualDeviceManager::parseModule(
	AutoPtr<AbstractConfiguration> cfg,
	const ModuleID &moduleID)
{
	ModuleType type = ModuleType::parse(cfg->getString("type"));
	VirtualModule::Ptr virtualModule = new VirtualModule(type);

	virtualModule->setModuleID(moduleID);
	virtualModule->setMin(cfg->getDouble("min", 0));
	virtualModule->setMax(cfg->getDouble("max", 100));
	virtualModule->setGenerator(cfg->getString("generator", ""));
	virtualModule->setReaction(cfg->getString("reaction", "none"));

	return virtualModule;
}

void VirtualDeviceManager::setConfigFile(const string &file)
{
	m_configFile = file;
}

void VirtualDeviceManager::installVirtualDevices()
{
	logger().information("loading configuration from: " + m_configFile);
	AutoPtr<AbstractConfiguration> cfg = new IniFileConfiguration(m_configFile);

	m_requestDeviceList =
		cfg->getBool("virtual-devices.request.device_list", true);

	for (int i = 0; cfg->has("virtual-device" + to_string(i) + ".enable"); ++i) {
		const string &prefix = "virtual-device" + to_string(i);

		if (!cfg->getBool(prefix + ".enable", false))
			continue;

		try {
			VirtualDevice::Ptr device = parseDevice(
				cfg->createView(prefix));
			registerDevice(device);
		}
		catch (const Exception &ex) {
			logger().log(ex, __FILE__, __LINE__);
			logger().error(
				"virtual device was not parsed or registered successfully",
				__FILE__, __LINE__
			);
			continue;
		}
	}

	logger().information(
		"loaded "
		+ to_string(m_virtualDevicesMap.size())
		+ " virtual devices",
		__FILE__, __LINE__
	);
}

void VirtualDeviceManager::dispatchNewDevice(VirtualDevice::Ptr device)
{
	const auto description = DeviceDescription::Builder()
		.id(device->id())
		.type(device->vendorName(), device->productName())
		.modules(device->moduleTypes())
		.refreshTime(device->refresh())
		.build();

	dispatch(new NewDeviceCommand(description));
}

void VirtualDeviceManager::doListenCommand(
	const GatewayListenCommand::Ptr)
{
	FastMutex::ScopedLock guard(m_lock);
	for (auto &item : m_virtualDevicesMap) {
		if (!deviceCache()->paired(item.first))
			dispatchNewDevice(item.second);
	}
}

void VirtualDeviceManager::doDeviceAcceptCommand(
		const DeviceAcceptCommand::Ptr cmd)
{
	FastMutex::ScopedLock guard(m_lock);
	auto it = m_virtualDevicesMap.find(cmd->deviceID());
	if (it == m_virtualDevicesMap.end())
		throw NotFoundException("accept " + cmd->deviceID().toString());

	if (deviceCache()->paired(cmd->deviceID())) {
		logger().warning(
			"ignoring accept for paired device "
			+ cmd->deviceID().toString(),
			__FILE__, __LINE__
		);
	}

	deviceCache()->markPaired(cmd->deviceID());
	m_pollingKeeper.schedule(it->second);
}

void VirtualDeviceManager::doUnpairCommand(
		const DeviceUnpairCommand::Ptr cmd)
{
	FastMutex::ScopedLock guard(m_lock);
	auto it = m_virtualDevicesMap.find(cmd->deviceID());
	if (it == m_virtualDevicesMap.end()) {
		logger().warning(
			"unpairing device that is not registered: "
			+ cmd->deviceID().toString(),
			__FILE__, __LINE__
		);

		return;
	}

	if (!deviceCache()->paired(cmd->deviceID())) {
		logger().warning(
			"unpairing device that is not paired: "
			+ cmd->deviceID().toString(),
			__FILE__, __LINE__
		);
	}

	deviceCache()->markUnpaired(cmd->deviceID());
	m_pollingKeeper.cancel(cmd->deviceID());
}

void VirtualDeviceManager::doSetValueCommand(
	const DeviceSetValueCommand::Ptr cmd)
{
	FastMutex::ScopedLock guard(m_lock);
	auto it = m_virtualDevicesMap.find(cmd->deviceID());
	if (it == m_virtualDevicesMap.end())
		throw NotFoundException("set-value: " + cmd->deviceID().toString());

	for (auto &item : it->second->modules()) {
		if (item->moduleID() == cmd->moduleID()) {
			if (item->reaction() == VirtualModule::REACTION_NONE) {
				throw InvalidAccessException(
					"cannot set-value: " + cmd->deviceID().toString());
			}
		}
	}

	if (!it->second->modifyValue(cmd->moduleID(), cmd->value())) {
		throw IllegalStateException(
			"set-value: " + cmd->deviceID().toString());
	}

	logger().debug(
		"module "
		+ cmd->moduleID().toString()
		+ " is set to value "
		+ to_string(cmd->value()),
		__FILE__, __LINE__
	);
}

void VirtualDeviceManager::handleGeneric(const Command::Ptr cmd, Result::Ptr result)
{
	if (cmd->is<GatewayListenCommand>())
		doListenCommand(cmd.cast<GatewayListenCommand>());
	else if (cmd->is<DeviceSetValueCommand>())
		doSetValueCommand(cmd.cast<DeviceSetValueCommand>());
	else if (cmd->is<DeviceUnpairCommand>())
		doUnpairCommand(cmd.cast<DeviceUnpairCommand>());
	else if (cmd->is<DeviceAcceptCommand>())
		doDeviceAcceptCommand(cmd.cast<DeviceAcceptCommand>());
	else
		DeviceManager::handleGeneric(cmd, result);
}

void VirtualDeviceManager::handleRemoteStatus(
	const DevicePrefix &prefix,
	const set<DeviceID> &devices,
	const DeviceStatusHandler::DeviceValues &values)
{
	DeviceManager::handleRemoteStatus(prefix, devices, values);
	scheduleAllEntries();
}

void VirtualDeviceManager::scheduleAllEntries()
{
	FastMutex::ScopedLock guard(m_lock);

	for (auto &item : m_virtualDevicesMap) {
		if (deviceCache()->paired(item.first))
			m_pollingKeeper.schedule(item.second);
		else
			m_pollingKeeper.cancel(item.first);
	}
}

void VirtualDeviceManager::run()
{
	StopControl::Run run(m_stopControl);

	while (run) {
		scheduleAllEntries();
		run.waitStoppable(DEFAULT_REFRESH_SECS * Timespan::SECONDS);
	}

	m_pollingKeeper.cancelAll();
}

void VirtualDeviceManager::stop()
{
	DeviceManager::stop();
	answerQueue().dispose();
}
