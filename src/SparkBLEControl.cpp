/*
 * SparkBLEControl.cpp
 *
 *  Created on: 19.08.2021
 *      Author: stangreg
 */

#include "SparkBLEControl.h"

//ClientCallbacks SparkBLEControl::clientCB;

SparkBLEControl::SparkBLEControl() {
	//advDevCB = new AdvertisedDeviceCallbacks();
	advDevice = new NimBLEAdvertisedDevice();
	spark_dc = nullptr;

}

SparkBLEControl::SparkBLEControl(SparkDataControl *dc) {
	//advDevCB = new AdvertisedDeviceCallbacks();
	advDevice = new NimBLEAdvertisedDevice();
	spark_dc = dc;

}

SparkBLEControl::~SparkBLEControl() {
	//if(advDevCB) delete advDevCB;
	if (advDevice)
		delete advDevice;
}

// Initializing BLE connection with NimBLE
void SparkBLEControl::initBLE(notify_callback notifyCallback) {
	//NimBLEDevice::init("");
	notifyCB = notifyCallback;

	/** Optional: set the transmit power, default is 3db */
	NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */

	/** create new scan */
	NimBLEScan *pScan = NimBLEDevice::getScan();

	/** create a callback that gets called when advertisers are found */
	pScan->setAdvertisedDeviceCallbacks(this, false);

	/** Set scan interval (how often) and window (how long) in milliseconds */
	pScan->setInterval(45);
	pScan->setWindow(15);

	/** Active scan will gather scan response data from advertisers
	 but will use more energy from both devices
	 */
	pScan->setActiveScan(true);
	/** Start scanning for advertisers for the scan time specified (in seconds) 0 = forever
	 Optional callback for when scanning stops.
	 */
	Serial.println("Starting scan");
	pScan->start(scanTime, scanEndedCB);
}

void SparkBLEControl::setAdvertisedDevice(NimBLEAdvertisedDevice *device) {
	advDevice = device;
}


void SparkBLEControl::scanEndedCB(NimBLEScanResults results) {
	Serial.println("Scan ended.");
}

void SparkBLEControl::startScan() {
	NimBLEDevice::getScan()->start(scanTime, scanEndedCB);
	Serial.println("Scan initiated");
}

bool SparkBLEControl::connectToServer() {
	/** Check if we have a client we should reuse first **/
	if (NimBLEDevice::getClientListSize()) {
		/** Special case when we already know this device, we send false as the
		 second argument in connect() to prevent refreshing the service database.
		 This saves considerable time and power.
		 */
		pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
		if (pClient) {
			if (!pClient->connect(advDevice, false)) {
				Serial.println("Reconnect failed");
				isAmpConnected_ = false;
				return false;
			}
			Serial.println("Reconnected client");
		}
		/** We don't already have a client that knows this device,
		 we will check for a client that is disconnected that we can use.
		 */
		else {
			pClient = NimBLEDevice::getDisconnectedClient();
		}
	}

	/** No client to reuse? Create a new one. */
	if (!pClient) {
		if (NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) {
			Serial.println(
					"Max clients reached - no more connections available");
			isAmpConnected_ = false;
			return false;
		}

		pClient = NimBLEDevice::createClient();

		pClient->setClientCallbacks(this, false);
		/** Set initial connection parameters: These settings are 15ms interval, 0 latency, 120ms timout.
		 These settings are safe for 3 clients to connect reliably, can go faster if you have less
		 connections. Timeout should be a multiple of the interval, minimum is 100ms.
		 Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 51 * 10ms = 510ms timeout
		 */
		pClient->setConnectionParams(12, 12, 0, 51);
		//pClient->setConnectionParams(40, 80, 5, 51);
		/** Set how long we are willing to wait for the connection to complete (seconds), default is 30. */
		pClient->setConnectTimeout(5);
		if (!pClient->connect(advDevice)) {
			/** Created a client but failed to connect, don't need to keep it as it has no data */
			NimBLEDevice::deleteClient(pClient);
			Serial.println("Failed to connect, deleted client");
			isAmpConnected_ = false;
			return false;
		}
	}

	if (!pClient->isConnected()) {
		if (!pClient->connect(advDevice)) {
			Serial.println("Failed to connect");
			isAmpConnected_ = false;
			return false;
		}
	}

	Serial.print("Connected to: ");
	Serial.println(pClient->getPeerAddress().toString().c_str());
	isAmpConnected_ = true;
	return true;
}

bool SparkBLEControl::subscribeToNotifications(notify_callback notifyCallback) {

	// Subscribe to notifications from Spark
	NimBLERemoteService *pSvc = nullptr;
	NimBLERemoteCharacteristic *pChr = nullptr;
	NimBLERemoteDescriptor *pDsc = nullptr;

	if (pClient) {
		pSvc = pClient->getService(SPARK_BLE_SERVICE_UUID);
		if (pSvc) { // make sure it's not null
			pChr = pSvc->getCharacteristic(SPARK_BLE_NOTIF_CHAR_UUID);

			if (pChr) { // make sure it's not null
				if (pChr->canNotify()) {
					Serial.printf(
							"Subscribing to service notifications of %s\n",
							SPARK_BLE_NOTIF_CHAR_UUID);
					Serial.println("Notifications turned on");
					// Descriptor 2902 needs to be activated in order to receive notifications
					pChr->getDescriptor(BLEUUID((uint16_t) 0x2902))->writeValue(
							(uint8_t*) notificationOn, 2, true);
					// Subscribing to Spark characteristic
					if (!pChr->subscribe(true, notifyCB)) {
						Serial.println("Subscribe failed, disconnecting");
						// Disconnect if subscribe failed
						pClient->disconnect();
						return false;
					}
				}

			} else {
				Serial.printf("%s characteristic not found.\n",
				SPARK_BLE_NOTIF_CHAR_UUID);
				return false;
			}

			Serial.println("Done with this device.");
			return true;
		} // pSrv
		else {
			Serial.printf("Service %s not found.\n", SPARK_BLE_SERVICE_UUID);
			return false;
		}
	} // pClient
	else {
		Serial.print("Client not found! Need reconnection");
		isAmpConnected_ = false;
		return false;
	}
}

// To send messages to Spark via Bluetooth LE
bool SparkBLEControl::writeBLE(std::vector<ByteVector> cmd, bool response) {
	if (pClient && pClient->isConnected()) {

		NimBLERemoteService *pSvc = nullptr;
		NimBLERemoteCharacteristic *pChr = nullptr;

		pSvc = pClient->getService(SPARK_BLE_SERVICE_UUID);
		if (pSvc) {
			pChr = pSvc->getCharacteristic(SPARK_BLE_WRITE_CHAR_UUID);

			if (pChr) {
				if (pChr->canWrite()) {
					std::vector<ByteVector> packets;
					// Spark messages are sent in chunks of 173 (0xAD) bytes, so we do the same.
					int max_send_size = 173;
					int curr_pos;

					for (auto block : cmd) {

						// This it to split messages into sizes of max. max_send_size.
						// As we have chosen 173, usually no further splitting is requried.
						// SparkMessage already creates messages split into 173 byte chunks
						DEBUG_PRINTLN("Sending packet:");
						for (auto byte: block){
							DEBUG_PRINTF("%s",SparkHelper::intToHex(byte));
						}
						DEBUG_PRINTLN();
						curr_pos = 0;

						int packetsToSend = (int) ceil(
								(double) block.size() / max_send_size);
						ByteVector packet;
						packet.clear();

						auto start = block.begin();
						auto end = block.end();

						// Splitting the message
						while (start != end) {
							auto next =
									std::distance(start, end) >= max_send_size ?
											start + max_send_size : end;

							packet.assign(start, next);
							packets.push_back(packet);
							start = next;
						} // While not at
					}

					// Send each packet to Spark
					for (auto packet : packets) {
						DEBUG_PRINTLN("Trying to send package");
						if (pChr->writeValue(packet.data(), packet.size(),
								response)) {
							// Delay seems to be required in order to not lose any packages.
							// Seems to be more stable with a short delay
							delay(10);
						} else {
							Serial.println("There was an error with writing!");
							// Disconnect if write failed
							pClient->disconnect();
							isAmpConnected_ = false;
							return false;
						}
					} //For each packet

				}  // if can write
			} // if pChr
			else {
				Serial.printf("Characteristic %s not found.\n",
				SPARK_BLE_WRITE_CHAR_UUID);
			}
		} // if pSvc
		else {
			Serial.printf("%s service not found.\n", SPARK_BLE_SERVICE_UUID);
		}
		DEBUG_PRINTLN("Done with this command!");
		return true;
	} else {
		isAmpConnected_ = false;
		return false;
	}
}

void SparkBLEControl::onResult(NimBLEAdvertisedDevice *advertisedDevice) {

	if (advertisedDevice->isAdvertisingService(
			NimBLEUUID(SPARK_BLE_SERVICE_UUID))) {
		Serial.println("Found Spark, connecting.");
		/** stop scan before connecting */
		//Commented as workaround, might need to get back here, is currently with DataControl;
		NimBLEDevice::getScan()->stop();
		/** Save the device reference in a global for the client to use*/
		setAdvertisedDevice(advertisedDevice);
		/** Ready to connect now */
		isConnectionFound_ = true;
	}
}

void SparkBLEControl::startServer() {
	Serial.println("Starting NimBLE Server");

	/** sets device name */
	NimBLEDevice::init("Spark 40 BLE");

	/** Optional: set the transmit power, default is 3db */
	NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
	NimBLEDevice::setSecurityAuth(
			/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/BLE_SM_PAIR_AUTHREQ_SC);

	pServer = NimBLEDevice::createServer();
	pServer->setCallbacks(this);

	NimBLEService *pSparkService = pServer->createService(
	SPARK_BLE_SERVICE_UUID);
	NimBLECharacteristic *pSparkWriteCharacteristic =
			pSparkService->createCharacteristic(
			SPARK_BLE_WRITE_CHAR_UUID,
					NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
							| NIMBLE_PROPERTY::WRITE_NR);

	uint8_t initialWriteValue[] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
			0x77, 0x77, 0x77 };

	pSparkWriteCharacteristic->setValue(initialWriteValue);
	pSparkWriteCharacteristic->setCallbacks(this);

	NimBLECharacteristic *pSparkNotificationCharacteristic =
			pSparkService->createCharacteristic(
			SPARK_BLE_NOTIF_CHAR_UUID,
					NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
	uint8_t initialNotificationValue[] = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			0x88, 0x88, 0x88, 0x88 };
	pSparkNotificationCharacteristic->setValue(initialNotificationValue);
	pSparkNotificationCharacteristic->setCallbacks(this);

	/** Start the services when finished creating all Characteristics and Descriptors */
	pSparkService->start();
	//pSparkNotificationService->start();
	NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

	//uint8_t Adv_DATA[] = {0xee, 0x03, 0x08, 0xEB, 0xED, 0x78, 0x0A, 0x6E};
	//BLEAdvertisementData oAdvertisementCustom = BLEAdvertisementData();
	//oAdvertisementCustom.setManufacturerData(std::string((char *)&Adv_DATA[0], 8)); // 8 is length of Adv_DATA
	//oAdvertisementCustom.setPartialServices(NimBLEUUID(SPARK_BLE_SERVICE_UUID));
	//oAdvertisementCustom.setFlags(0x06);
	//pAdvertising->setAdvertisementData(oAdvertisementCustom);
	pAdvertising->addServiceUUID(pSparkService->getUUID());

	/** Add the services to the advertisment data **/
	/** If your device is battery powered you may consider setting scan response
	 *  to false as it will extend battery life at the expense of less data sent.
	 */
	pAdvertising->setScanResponse(true);
	pAdvertising->start();

	Serial.println("Advertising Started");

}

void SparkBLEControl::onWrite(NimBLECharacteristic *pCharacteristic) {
	DEBUG_PRINT(pCharacteristic->getUUID().toString().c_str());
	DEBUG_PRINTLN(": onWrite()");
	std::string rxValue = pCharacteristic->getValue();
	ByteVector byteVector;
	byteVector.clear();

	if (rxValue.length() > 0) {
		for (int i = 0; i < rxValue.length(); i++) {
			byteVector.push_back((byte) (rxValue[i]));
		}
	}

	if (spark_dc->processSparkData(byteVector) == MSG_PROCESS_RES_INITIAL) {
		sendInitialNotification();
	}
}

void SparkBLEControl::onSubscribe(NimBLECharacteristic *pCharacteristic,
		ble_gap_conn_desc *desc, uint16_t subValue) {
	std::string str = "Address: ";
	str += std::string(NimBLEAddress(desc->peer_ota_addr)).c_str();
	if (subValue == 0) {
		str += " Unsubscribed to ";
	} else if (subValue == 1) {
		str += " Subscribed to notfications for ";
	} else if (subValue == 2) {
		str += " Subscribed to indications for ";
	} else if (subValue == 3) {
		str += " Subscribed to notifications and indications for ";
	}
	str += std::string(pCharacteristic->getUUID());

	Serial.println(str.c_str());
}
;

void SparkBLEControl::notifyClients(ByteVector msg) {
	// BLE mode, commented for testing
	/*
	NimBLEService *pSvc = pServer->getServiceByUUID(SPARK_BLE_SERVICE_UUID);
	if (pSvc) {
		NimBLECharacteristic *pChr = pSvc->getCharacteristic(
		SPARK_BLE_NOTIF_CHAR_UUID);
		if (pChr) {
			DEBUG_PRINTLN("Sending data:");
			DEBUG_PRINTVECTOR(msg);
			DEBUG_PRINTLN();
			pChr->setValue(&msg.data()[0], msg.size());
			pChr->notify(true);
		}
	 }*/
	Serial.println("Sending message");
	for (byte by : msg) {
		if (by < 16) {
			Serial.print("0");
			serialBT.write('0');
		}
		Serial.print(by, HEX);
		serialBT.write(by);
	}
	Serial.println();
}

void SparkBLEControl::sendInitialNotification() {

	// TODO: Find and process real messages as requested by app

	// When connecting app, we need to send a set of notifications for a successful connection
	// Firmware version
	// 03 2F (First request)
	ByteVector msg_firmware = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x1D, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x01, 0x01,
			0x29, 0x03, 0x2F, 0x01, 0x4E, 0x01, 0x05, 0x04, 0x66, 0xF7 };

	// Unknown message
	// 03 2A (Second request)
	ByteVector msg_interim1 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x1E, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x01, 0x02,
			0x2D, 0x03, 0x2A, 0x0D, 0x14, 0x7D, 0x4C, 0x07, 0x5A, 0x58, 0xF7 };

	// Current preset number
	// 03 10 (Third request)
	ByteVector msg_presetNum = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x1A, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x01, 0x03,
			0x00, 0x03, 0x10, 0x00, 0x00, 0x00, 0xF7 };

	//Current preset
	// 03 01 (Fourth request), divided into 7 segments
	ByteVector msg_preset_1_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x6A,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xF0, 0x01,
			0x04, 0x6A, 0x03, 0x01, 0x20, 0x0F, 0x00, 0x19, 0x01, 0x00, 0x59,
			0x24, 0x00, 0x37, 0x34, 0x32, 0x35, 0x32, 0x31, 0x31, 0x00, 0x37,
			0x2D, 0x43, 0x32, 0x41, 0x41, 0x2D, 0x00, 0x34, 0x31, 0x33, 0x35,
			0x2D, 0x38, 0x46, 0xF7, 0xF0, 0x01, 0x04, 0x19, 0x03, 0x01, 0x00,
			0x0F, 0x01, 0x19, 0x39, 0x32, 0x2D, 0x37, 0x00, 0x43, 0x46, 0x44,
			0x41, 0x30, 0x31, 0x46, 0x10, 0x35, 0x31, 0x36, 0x37, 0x27, 0x31,
			0x2D, 0x20, 0x43, 0x6C, 0x65, 0x61, 0x6E, 0x23, 0x30, 0xF7, 0xF0,
			0x01, 0x04, 0x42, 0x03, 0x01, 0x20, 0x0F, 0x02, 0x19, 0x2E, 0x37 };

	ByteVector msg_preset_2_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x6A,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x31, 0x40,
			0x2D, 0x43, 0x6C, 0x65, 0x61, 0x6E, 0x28, 0x00, 0x69, 0x63, 0x6F,
			0x6E, 0x2E, 0x70, 0x6E, 0x4A, 0x67, 0x4A, 0x42, 0x70, 0x00, 0x00,
			0x17, 0xF7, 0xF0, 0x01, 0x04, 0x09, 0x03, 0x01, 0x08, 0x0F, 0x03,
			0x19, 0x2E, 0x62, 0x69, 0x61, 0x00, 0x73, 0x2E, 0x6E, 0x6F, 0x69,
			0x73, 0x65, 0x30, 0x67, 0x61, 0x74, 0x65, 0x43, 0x13, 0x00, 0x3B,
			0x11, 0x4A, 0x3D, 0x75, 0x6E, 0x43, 0x01, 0xF7, 0xF0, 0x01, 0x04,
			0x12, 0x03, 0x01, 0x58, 0x0F, 0x04, 0x19, 0x11, 0x4A, 0x3E, 0x29,
			0x59, 0x2F, 0x12, 0x02, 0x11, 0x4A, 0x3F, 0x00, 0x04, 0x00 };

	ByteVector msg_preset_3_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x6A,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x43,
			0x6F, 0x6D, 0x70, 0x40, 0x72, 0x65, 0x73, 0x73, 0x6F, 0x72, 0x43,
			0xF7, 0xF0, 0x01, 0x04, 0x26, 0x03, 0x01, 0x68, 0x0F, 0x05, 0x19,
			0x12, 0x00, 0x11, 0x4A, 0x6E, 0x3E, 0x2A, 0x3B, 0x10, 0x01, 0x11,
			0x4A, 0x14, 0x3F, 0x7F, 0x47, 0x4B, 0x27, 0x42, 0x6F, 0x60, 0x6F,
			0x73, 0x74, 0x65, 0x72, 0x43, 0x11, 0xF7, 0xF0, 0x01, 0x04, 0x1D,
			0x03, 0x01, 0x30, 0x0F, 0x06, 0x19, 0x00, 0x11, 0x4A, 0x3F, 0x08,
			0x0F, 0x1F, 0x78, 0x24, 0x54, 0x77, 0x69, 0x36, 0x6E, 0x43, 0x15,
			0x00, 0x11, 0x4A, 0x3F, 0x34, 0x1D, 0x09, 0x79, 0x01, 0x11 };

	ByteVector msg_preset_4_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x6A,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x3E, 0xF7,
			0xF0, 0x01, 0x04, 0x46, 0x03, 0x01, 0x28, 0x0F, 0x07, 0x19,
			0x61, 0x74, 0x0A, 0x02, 0x1B, 0x11, 0x4A, 0x3E, 0x41, 0x70, 0x54,
			0x03, 0x2B, 0x11, 0x4A, 0x3E, 0x7B, 0x13, 0x49, 0x04, 0x53, 0x11,
			0x4A, 0x3F, 0x20, 0x79, 0x53, 0x2C, 0xF7, 0xF0, 0x01, 0x04, 0x37,
			0x03,
			0x01, 0x00, 0x0F, 0x08, 0x19, 0x43, 0x68, 0x6F, 0x72, 0x00, 0x75,
			0x73, 0x41, 0x6E, 0x61, 0x6C, 0x6F, 0x36, 0x67, 0x42, 0x14,
			0x00, 0x11, 0x4A, 0x3E, 0x35, 0x41, 0x15, 0x32, 0x01, 0x11, 0x4A,
			0x3F, 0xF7, 0xF0, 0x01, 0x04, 0x2C, 0x03, 0x01, 0x00, 0x0F, 0x09 };

	ByteVector msg_preset_5_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x6A,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x11, 0x5B,
			0x1E, 0x02, 0x23, 0x11, 0x4A, 0x3E, 0x5D, 0x49, 0x44, 0x03, 0x4B,
			0x11, 0x4A, 0x3E, 0x00, 0x00, 0x00, 0x29, 0x00, 0x44, 0x65, 0x6C,
			0x61, 0x79, 0x4D, 0x6F, 0xF7, 0xF0, 0x01, 0x04, 0x76, 0x03, 0x01,
			0x60, 0x0F, 0x0A, 0x19, 0x6E, 0x6F, 0x43, 0x15, 0x66, 0x00, 0x11,
			0x4A, 0x3E, 0x1F, 0x31, 0x20, 0x66, 0x01, 0x11, 0x4A, 0x3E, 0x6E,
			0x24, 0x61, 0x16, 0x02, 0x11, 0x4A, 0x3E, 0x7B, 0x24, 0x57, 0xF7,
			0xF0, 0x01, 0x04, 0x59, 0x03, 0x01, 0x30, 0x0F, 0x0B, 0x19, 0x03,
			0x11, 0x4A, 0x3F, 0x34, 0x1B, 0x55, 0x6A, 0x04, 0x11, 0x4A };

	ByteVector msg_preset_6_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x6A,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x09, 0x00,
			0x00, 0x00, 0x2B, 0x62, 0x69, 0x61, 0x00, 0x73, 0x2E, 0x72, 0x65,
			0x76, 0x65, 0x72, 0xF7, 0xF0, 0x01, 0x04, 0x08, 0x03, 0x01, 0x30,
			0x0F, 0x0C, 0x19, 0x62, 0x43, 0x18, 0x00, 0x0B, 0x11, 0x4A, 0x3E,
			0x2D, 0x30, 0x27, 0x01, 0x3B, 0x11, 0x4A, 0x3E, 0x28, 0x19, 0x3B,
			0x02, 0x3B, 0x11, 0x4A, 0x3E, 0x60, 0x17, 0x32, 0x03, 0xF7, 0xF0,
			0x01, 0x04, 0x24, 0x03, 0x01, 0x18, 0x0F, 0x0D, 0x19, 0x11, 0x4A,
			0x3F, 0x31, 0x5B, 0x16, 0x20, 0x04, 0x11, 0x4A, 0x3E, 0x79, 0x5B,
			0x79, 0x7A, 0x05, 0x11, 0x4A, 0x3E, 0x6E, 0x59, 0x4A, 0x38 };

	ByteVector msg_preset_7_7 = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x2C,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x11, 0x4A,
			0x3E, 0x19, 0xF7, 0xF0, 0x01, 0x04, 0x68, 0x03, 0x01, 0x58, 0x0F,
			0x0E, 0x0A, 0x19, 0x1A, 0x07, 0x11, 0x05, 0x4A, 0x3F, 0x00, 0x00,
			0x00, 0x5E, 0xF7 };

	// Serial number of the Spark. Sending fake one.
	// Serial number (Fifth request)
	ByteVector msg_serial = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x29, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x01, 0x05,
			0x3F, 0x03, 0x23, 0x02, 0x0D, 0x2D, 0x53, 0x39, 0x39, 0x39, 0x43,
			0x00, 0x39, 0x39, 0x39, 0x42, 0x39, 0x39, 0x39, 0x01, 0x77, 0xF7 };

	// License key Acknowledge
	// Sixth request
	ByteVector msg_license_ack = { 0x01, 0xFE, 0x00, 0x00, 0x41, 0xFF, 0x19,
			0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x01, 0x06,
			0x00, 0x04, 0x70, 0x00, 0x00, 0xF7 };

	int delayValue = 0;
	//if (pServer->getConnectedCount()) {
		DEBUG_PRINTLN("Sending notifications...");
		notificationCount++;
		switch (notificationCount) {
		default:
			notificationCount = 1;
			/* no break */
		case 1:
			notifyClients(msg_firmware);
			delay(delayValue);
			break;
		case 2:
			notifyClients(msg_interim1);
			delay(delayValue);
			break;
		case 3:
			notifyClients(msg_presetNum);
			delay(delayValue);
			break;
		case 4:
			notifyClients(msg_preset_1_7);
			delay(delayValue);
			notifyClients(msg_preset_2_7);
			delay(delayValue);
			notifyClients(msg_preset_3_7);
			delay(delayValue);
			notifyClients(msg_preset_4_7);
			delay(delayValue);
			notifyClients(msg_preset_5_7);
			delay(delayValue);
			notifyClients(msg_preset_6_7);
			delay(delayValue);
			notifyClients(msg_preset_7_7);
			delay(delayValue);
			break;
		case 5:
			notifyClients(msg_serial);
			delay(delayValue);
			break;
		}

	//} // if server connected
}


void SparkBLEControl::onConnect(NimBLEServer *pServer,
		ble_gap_conn_desc *desc) {
	isAppConnected_ = true;
	Serial.println("Multi-connect support: start advertising");
	//	pServer->updateConnParams(desc->conn_handle, 40, 80, 5, 51);
	NimBLEDevice::startAdvertising();
}

void SparkBLEControl::onDisconnect(NimBLEServer *pServer) {
	Serial.println("Client disconnected");
	isAppConnected_ = false;
	notificationCount = 0;
	Serial.println("Start advertising");
	NimBLEDevice::startAdvertising();
}
;

void SparkBLEControl::onDisconnect(NimBLEClient *pClient) {
	isAmpConnected_ = false;
	isConnectionFound_ = false;
	if (!(NimBLEDevice::getScan()->isScanning())) {
		startScan();
	}
	NimBLEClientCallbacks::onDisconnect(pClient);
}

void SparkBLEControl::stopScan() {
	if (isScanning()) {
		Serial.println("Scan stopped");
		NimBLEDevice::getScan()->stop();
	} else {
		Serial.print("Scan is not running");
	}

}

void SparkBLEControl::startBTClassic() {
	//Serial.printf("Initializing BT Classic with name %s",
	//		SPARK_BT_NAME.c_str());
	serialBT.begin("Spark 40 Audio", false);
}






