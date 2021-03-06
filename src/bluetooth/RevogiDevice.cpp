#include "bluetooth/RevogiDevice.h"
#include "bluetooth/RevogiSmartCandle.h"
#include "bluetooth/RevogiSmartLite.h"
#include "bluetooth/RevogiSmartPlug.h"

using namespace BeeeOn;
using namespace Poco;
using namespace std;

const UUID RevogiDevice::ACTUAL_VALUES_GATT = UUID("0000fff4-0000-1000-8000-00805f9b34fb");
const UUID RevogiDevice::WRITE_VALUES_GATT = UUID("0000fff3-0000-1000-8000-00805f9b34fb");
const UUID RevogiDevice::UUID_DEVICE_NAME = UUID("0000fff6-0000-1000-8000-00805f9b34fb");
const string RevogiDevice::VENDOR_NAME = "Revogi";
const vector<unsigned char> RevogiDevice::NOTIFY_DATA = {
	0x0f, 0x05, 0x04, 0x00, 0x00, 0x00, 0x05, 0xff, 0xff
};

RevogiDevice::RevogiDevice(
		const MACAddress& address,
		const Timespan& timeout,
		const RefreshTime& refresh,
		const string& productName,
		const list<ModuleType>& moduleTypes,
		const HciInterface::Ptr hci):
	BLESmartDevice(address, timeout, refresh, hci),
	m_productName(productName),
	m_moduleTypes(moduleTypes)
{
}

RevogiDevice::~RevogiDevice()
{
}

list<ModuleType> RevogiDevice::moduleTypes() const
{
	return m_moduleTypes;
}

string RevogiDevice::vendor() const
{
	return VENDOR_NAME;
}

string RevogiDevice::productName() const
{
	return m_productName;
}

bool RevogiDevice::pollable() const
{
	return true;
}

void RevogiDevice::poll(Distributor::Ptr distributor)
{
	SynchronizedObject::ScopedLock guard(*this);

	HciConnection::Ptr conn = m_hci->connect(m_address, m_timeout);
	vector<unsigned char> values = conn->notifiedWrite(
		ACTUAL_VALUES_GATT, WRITE_VALUES_GATT, NOTIFY_DATA, m_timeout);

	distributor->exportData(parseValues(values));
}

void RevogiDevice::sendWriteRequest(
		HciConnection::Ptr conn,
		vector<unsigned char> payload,
		const unsigned char checksum)
{
	prependHeader(payload);
	appendFooter(payload, checksum);

	conn->write(WRITE_VALUES_GATT, payload);
}

void RevogiDevice::appendFooter(
		vector<unsigned char>& payload,
		const unsigned char checksum) const
{
	payload.insert(payload.end(), {checksum, 0xff, 0xff});
}


bool RevogiDevice::match(const string& modelID)
{
	return modelID == "Model Number";
}

RevogiDevice::Ptr RevogiDevice::createDevice(
		const MACAddress& address,
		const Timespan& timeout,
		const RefreshTime& refresh,
		const HciInterface::Ptr hci,
		HciConnection::Ptr conn)
{
	vector<unsigned char> data = conn->read(UUID_DEVICE_NAME);
	string modelID(data.begin(), data.end());

	if (modelID == RevogiSmartLite::LIGHT_NAME)
		return new RevogiSmartLite(address, timeout, refresh, hci);
	else if (RevogiSmartCandle::LIGHT_NAMES.find(modelID) != RevogiSmartCandle::LIGHT_NAMES.end())
		return new RevogiSmartCandle(modelID, address, timeout, refresh, hci);
	else if (modelID == RevogiSmartPlug::PLUG_NAME)
		return new RevogiSmartPlug(address, timeout, refresh, hci);

	throw NotFoundException("device " + modelID + " not supported");
}
