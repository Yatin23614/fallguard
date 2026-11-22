#include "ble_alert.h"

#include <NimBLEDevice.h>
#include <string.h>

// NimBLE-Arduino 2.x API.
namespace {
NimBLECharacteristic* alertChr = nullptr;
NimBLECharacteristic* statusChr = nullptr;
NimBLECharacteristic* motionChr = nullptr;
volatile bool ackFlag = false;
volatile int clients = 0;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    ++clients;
    NimBLEDevice::startAdvertising();  // allow a second phone/caregiver
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    if (clients > 0) --clients;
    NimBLEDevice::startAdvertising();
  }
};

class AckCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    if (c->getValue().length() > 0) ackFlag = true;
  }
};
}  // namespace

void bleBegin(const char* name) {
  NimBLEDevice::init(name);
  NimBLEDevice::setPower(9);  // dBm
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  NimBLEService* svc = server->createService(FG_SERVICE_UUID);
  alertChr = svc->createCharacteristic(
      FG_ALERT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusChr = svc->createCharacteristic(
      FG_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  motionChr = svc->createCharacteristic(FG_MOTION_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* ack = svc->createCharacteristic(
      FG_ACK_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  ack->setCallbacks(new AckCB());
  alertChr->setValue("{}");
  statusChr->setValue("{}");
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(name);
  adv->addServiceUUID(FG_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
}

bool bleConnected() { return clients > 0; }

void bleSendAlert(const char* json) {
  alertChr->setValue((const uint8_t*)json, strlen(json));
  alertChr->notify();
}

void bleSendStatus(const char* json) {
  statusChr->setValue((const uint8_t*)json, strlen(json));
  statusChr->notify();
}

void bleSendMotion(const uint8_t* data, unsigned len) {
  motionChr->setValue(data, len);
  motionChr->notify();
}

bool bleTakeAck() {
  if (!ackFlag) return false;
  ackFlag = false;
  return true;
}
