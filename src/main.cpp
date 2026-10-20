#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include "usb/usb_host.h"
#include "wifi_secret.h"

namespace {

constexpr uint8_t kPrinterClass = 0x07;
constexpr uint8_t kPrinterSubclass = 0x01;
constexpr size_t kChunk = 4096;
constexpr uint32_t kJobIdleMs = 30000;

struct Printer {
  usb_host_client_handle_t client = nullptr;
  usb_device_handle_t device = nullptr;
  usb_transfer_t* out = nullptr;
  usb_transfer_t* waiting = nullptr;
  uint8_t iface = 0;
  uint8_t epOut = 0;
  uint8_t epIn = 0;
  uint8_t protocol = 0;
  uint8_t altSetting = 0;
  uint16_t mps = 64;
  volatile int pendingAddr = -1;
  volatile bool pendingGone = false;
  volatile bool xferDone = false;
  bool outBusy = false;
  bool ctrlBusy = false;
  bool closing = false;
  bool claimed = false;
  bool ready = false;
  uint32_t jobSent = 0;

  bool begin() {
    const usb_host_config_t host = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    if (usb_host_install(&host) != ESP_OK) return false;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = onEvent, .callback_arg = this},
    };
    return usb_host_client_register(&cfg, &client) == ESP_OK;
  }

  void poll() {
    if (!client) return;
    usb_host_lib_handle_events(0, nullptr);
    usb_host_client_handle_events(client, 1);
    if (pendingGone) {
      pendingGone = false;
      close();
    }
    if (closing && !outBusy) finishClose();
    if (pendingAddr >= 0 && !device && !closing) {
      uint8_t addr = pendingAddr;
      pendingAddr = -1;
      open(addr);
    }
  }

  bool beginJob() {
    if (!ready || !out || outBusy || closing) {
      Serial.println("job: printer not ready");
      return false;
    }
    if (!selectAlt()) return false;
    jobSent = 0;
    Serial.println("job: start");
    return true;
  }

  bool writeChunk(const uint8_t* data, size_t len) {
    if (!out || outBusy || closing) return false;
    size_t off = 0;
    while (off < len) {
      size_t n = len - off;
      if (n > out->data_buffer_size) n = out->data_buffer_size;
      memcpy(out->data_buffer, data + off, n);
      out->num_bytes = n;
      out->flags = 0;
      if (!submit(out, false, 20000)) {
        Serial.printf("job: write failed at %u status=%d\n", jobSent, outBusy ? -1 : out->status);
        return false;
      }
      off += n;
      jobSent += n;
    }
    return true;
  }

  void endJob() {
    if (!outBusy && !closing && out && jobSent && mps && (jobSent % mps) == 0) {
      out->num_bytes = 0;
      out->flags = 0;
      submit(out, false, 2000);
    }
    Serial.printf("job: done %u bytes\n", jobSent);
    jobSent = 0;
  }

  void abortJob() {
    endJob();
    if (outBusy || !out || closing) return;
    const uint8_t reset[] = {0x1b, '%', '-', '1', '2', '3', '4', '5', 'X', 0x1b, 'E'};
    writeChunk(reset, sizeof reset);
    endJob();
  }

  void report() {
    if (!client) return;
    uint8_t addrs[8] = {};
    int n = 0;
    usb_host_device_addr_list_fill(8, addrs, &n);
    Serial.printf("printer %s  devices=%d\n", n > 0 ? "HERE" : "GONE", n);
  }

 private:
  static void onEvent(const usb_host_client_event_msg_t* msg, void* arg) {
    auto* self = static_cast<Printer*>(arg);
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
      Serial.printf("event connect addr=%u\n", msg->new_dev.address);
      self->pendingAddr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
      Serial.println("event disconnect");
      self->pendingGone = true;
    }
  }

  static void onXfer(usb_transfer_t* t) {
    auto* self = static_cast<Printer*>(t->context);
    if (t == self->out) self->outBusy = false;
    else self->ctrlBusy = false;
    if (t == self->waiting) self->xferDone = true;
    else if (t != self->out) usb_host_transfer_free(t);
  }

  void pump() {
    usb_host_lib_handle_events(0, nullptr);
    usb_host_client_handle_events(client, 1);
    feedLoopWDT();
  }

  void recover(usb_transfer_t* t) {
    if (!t->device_handle) return;
    bool halted = false;
    if (!xferDone) pump();
    if (!xferDone && usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress) == ESP_OK) {
      halted = true;
      usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
      uint32_t start = millis();
      while (!xferDone && (int32_t)(millis() - start) < 200) pump();
    }
    if (halted || (xferDone && t->status != USB_TRANSFER_STATUS_COMPLETED &&
                   t->status != USB_TRANSFER_STATUS_NO_DEVICE))
      usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
  }

  bool submit(usb_transfer_t* t, bool control, int timeoutMs, bool* queued = nullptr) {
    if (control && ctrlBusy) {
      if (queued) *queued = false;
      return false;
    }
    xferDone = false;
    waiting = t;
    t->context = this;
    t->callback = onXfer;
    if (t == out) outBusy = true;
    else ctrlBusy = true;
    esp_err_t err = control ? usb_host_transfer_submit_control(client, t) : usb_host_transfer_submit(t);
    if (err != ESP_OK) {
      waiting = nullptr;
      if (t == out) outBusy = false;
      else ctrlBusy = false;
      if (queued) *queued = false;
      Serial.printf("usb submit err=%d ep=0x%02x\n", err, t->bEndpointAddress);
      return false;
    }
    if (queued) *queued = true;
    uint32_t start = millis();
    while (!xferDone && (int32_t)(millis() - start) < timeoutMs) pump();
    if (!(xferDone && t->status == USB_TRANSFER_STATUS_COMPLETED)) recover(t);
    waiting = nullptr;
    if (!xferDone) Serial.printf("usb stuck ep=0x%02x\n", t->bEndpointAddress);
    return xferDone && t->status == USB_TRANSFER_STATUS_COMPLETED;
  }

  bool control(uint8_t bm, uint8_t req, uint16_t value, uint16_t index, uint8_t* data, uint16_t length, int timeoutMs) {
    usb_transfer_t* t = nullptr;
    if (usb_host_transfer_alloc(8 + length, 0, &t) != ESP_OK) return false;
    t->device_handle = device;
    t->bEndpointAddress = 0;
    t->num_bytes = 8 + length;
    auto* s = reinterpret_cast<usb_setup_packet_t*>(t->data_buffer);
    s->bmRequestType = bm;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = length;
    bool queued = false;
    bool ok = submit(t, true, timeoutMs, &queued);
    if (ok && length && t->actual_num_bytes < 8 + length) ok = false;
    if (ok && data && length) memcpy(data, t->data_buffer + 8, length);
    else if (!ok && queued && xferDone) Serial.printf("usb control req=%u status=%d\n", req, t->status);
    if (!queued || xferDone) usb_host_transfer_free(t);
    return ok;
  }

  bool selectAlt() {
    if (!control(0x01, 0x0b, altSetting, iface, nullptr, 0, 1000)) return false;
    uint8_t got = 0xff;
    if (!control(0x81, 0x0a, 0, iface, &got, 1, 1000)) return false;
    Serial.printf("print: GET_INTERFACE alt=%u\n", got);
    return got == altSetting;
  }

  void open(uint8_t addr) {
    if (usb_host_device_open(client, addr, &device) != ESP_OK) {
      Serial.println("usb open failed");
      return;
    }
    const usb_device_desc_t* dev = nullptr;
    usb_host_get_device_descriptor(device, &dev);
    if (dev) Serial.printf("usb %04x:%04x\n", dev->idVendor, dev->idProduct);
    if (!findOut()) {
      Serial.printf("printer interface not found protocol=%u\n", protocol);
      close();
      return;
    }
    if (usb_host_interface_claim(client, device, iface, altSetting) != ESP_OK) {
      Serial.printf("usb claim failed iface=%u alt=%u protocol=%u\n", iface, altSetting, protocol);
      close();
      return;
    }
    claimed = true;
    if (usb_host_transfer_alloc(kChunk, 0, &out) != ESP_OK) {
      Serial.println("usb transfer alloc failed");
      close();
      return;
    }
    out->device_handle = device;
    out->bEndpointAddress = epOut;
    ready = true;
    Serial.printf("claimed iface=%u alt=%u protocol=%u epOut=0x%02x epIn=0x%02x\n", iface, altSetting, protocol, epOut, epIn);
  }

  bool findOut() {
    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(device, &cfg) != ESP_OK) return false;
    iface = 0;
    altSetting = 0;
    protocol = 0;
    epOut = 0;
    epIn = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(cfg);
    int left = cfg->wTotalLength;
    const usb_intf_desc_t* ifaceDesc = nullptr;
    while (left > 0) {
      auto* d = reinterpret_cast<const usb_standard_desc_t*>(p);
      if (d->bLength < 2 || d->bLength > left) break;
      if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE && d->bLength >= sizeof(usb_intf_desc_t)) {
        ifaceDesc = reinterpret_cast<const usb_intf_desc_t*>(d);
      } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && ifaceDesc &&
                 d->bLength >= sizeof(usb_ep_desc_t) && ifaceDesc->bInterfaceClass == kPrinterClass &&
                 ifaceDesc->bInterfaceSubClass == kPrinterSubclass) {
        auto* ep = reinterpret_cast<const usb_ep_desc_t*>(d);
        uint8_t proto = ifaceDesc->bInterfaceProtocol;
        if ((ep->bmAttributes & 0x03) == USB_BM_ATTRIBUTES_XFER_BULK && (proto == 1 || proto == 2)) {
          if ((epOut == 0 && epIn == 0) || proto > protocol) {
            iface = ifaceDesc->bInterfaceNumber;
            altSetting = ifaceDesc->bAlternateSetting;
            protocol = proto;
            epOut = 0;
            epIn = 0;
          }
          if (proto == protocol && ifaceDesc->bAlternateSetting == altSetting) {
            if (ep->bEndpointAddress & 0x80) epIn = ep->bEndpointAddress;
            else {
              epOut = ep->bEndpointAddress;
              mps = ep->wMaxPacketSize & 0x7ff;
            }
            Serial.printf("saw protocol=%u alt=%u ep=0x%02x mps=%u\n", proto, altSetting, ep->bEndpointAddress,
                          ep->wMaxPacketSize & 0x7ff);
          }
        }
      }
      p += d->bLength;
      left -= d->bLength;
    }
    return epOut != 0;
  }

  void close() {
    ready = false;
    if (outBusy) {
      closing = true;
      return;
    }
    finishClose();
  }

  void finishClose() {
    if (out) {
      usb_host_transfer_free(out);
      out = nullptr;
    }
    if (device) {
      if (claimed) usb_host_interface_release(client, device, iface);
      usb_host_device_close(client, device);
      device = nullptr;
    }
    claimed = false;
    closing = false;
    epOut = 0;
    epIn = 0;
    iface = 0;
    altSetting = 0;
    protocol = 0;
    mps = 64;
    Serial.println("usb closed");
  }
};

Printer printer;
WiFiServer jobServer(9100);
WiFiClient jobClient;
bool listening = false;
bool jobActive = false;
uint32_t jobIdleAt = 0;

void keepAlive(WiFiClient& client) {
  int fd = client.fd();
  if (fd < 0) return;
  int yes = 1, idle = 30, intvl = 10, cnt = 3;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("lj1010 print server");
  Serial.println("log cable: USB-C CH343, printer: the other USB-C");
  enableLoopWDT();
  if (!printer.begin()) Serial.println("usb host failed");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("lj1010");
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
  printer.poll();
  if (WiFi.status() == WL_CONNECTED) {
    if (!listening) {
      jobServer.begin();
      listening = true;
      Serial.print("listen 9100 ");
      Serial.println(WiFi.localIP());
    }
  } else if (listening) {
    jobServer.end();
    listening = false;
  }

  if (jobActive) {
    if (jobClient.available()) {
      uint8_t buf[1024];
      int n = jobClient.read(buf, sizeof buf);
      if (n > 0 && printer.writeChunk(buf, n)) jobIdleAt = millis();
      else if (n > 0) {
        printer.abortJob();
        jobClient.stop();
        jobActive = false;
      }
    } else if (!jobClient.connected()) {
      int fd = jobClient.fd(), err = 0;
      socklen_t errLen = sizeof err;
      if (fd >= 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) == 0 && err) printer.abortJob();
      else printer.endJob();
      jobClient.stop();
      jobActive = false;
    } else if ((int32_t)(millis() - jobIdleAt) >= (int32_t)kJobIdleMs) {
      printer.abortJob();
      jobClient.stop();
      jobActive = false;
    }
  } else if (listening) {
    WiFiClient next = jobServer.accept();
    if (next) {
      jobClient = next;
      jobClient.setNoDelay(true);
      keepAlive(jobClient);
      Serial.println("job: client");
      uint32_t wait = millis();
      while (!printer.ready && (int32_t)(millis() - wait) < 3000) {
        printer.poll();
        feedLoopWDT();
      }
      jobActive = printer.beginJob();
      if (jobActive) jobIdleAt = millis();
      else jobClient.stop();
    }
  }

  static uint32_t nextReport = 0;
  if (!jobActive && (int32_t)(millis() - nextReport) >= 0) {
    nextReport = millis() + 5000;
    printer.report();
  }
}
