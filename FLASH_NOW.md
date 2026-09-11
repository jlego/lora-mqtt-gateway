# 🚀 Flash Your LilyGo LoRa32 V2.1 Right Now!

Your firmware is **READY TO FLASH**! This is a fully working LoRa MQTT Gateway.

## ✅ What's Working

- ✅ **LoRa Radio** - SX1268 fully configured and operational
- ✅ **MQTT Bridging** - LoRa ↔ MQTT message forwarding
- ✅ **Serial Configuration** - Interactive menu to configure everything
- ✅ **WiFi** - Auto-reconnect with persistent settings
- ✅ **Repeater Mode** - Optionally repeat received packets
- ✅ **Statistics** - Real-time packet monitoring

## 📱 Quick Flash (3 Steps)

### 1️⃣ Open PlatformIO

In VS Code, open the PlatformIO sidebar (ant icon)

### 2️⃣ Click Upload

Click **"Upload"** under `lilygo_lora32_v21` environment

**OR** run in terminal:
```bash
pio run --target upload
```

### 3️⃣ Open Serial Monitor

Click **"Monitor"** or run:
```bash
pio device monitor
```

## 🎯 First Boot Setup

When you first power on, you'll see:

```
╔════════════════════════════════════════════════════════╗
║        MeshCore MQTT Gateway v1.0.0                    ║
╚════════════════════════════════════════════════════════╝

✓ Configuration loaded
✓ Generated Node ID: 0x12345678
Initializing SX1268 radio... success!
✓ Radio listening for packets

┌── LoRa Configuration ──────────────────────────────────┐
│ Frequency:        915.00 MHz                           │
│ Bandwidth:        125.0 kHz                            │
│ Spreading Factor: 7                                    │
│ Coding Rate:      4/5                                  │
│ TX Power:         20 dBm                               │
│ Sync Word:        0x12                                 │
│ CRC:              Enabled                              │
└────────────────────────────────────────────────────────┘

✓ Gateway started successfully!

Commands:
  'c' - Enter configuration menu
  's' - Show statistics
  'r' - Restart device
```

## ⚙️ Configure WiFi and MQTT

### Step 1: Press `c` to Enter Configuration

```
┌────────────────────────────────────────────────────────┐
│ MAIN MENU                                              │
├────────────────────────────────────────────────────────┤
│ 1. WiFi Settings                                       │
│ 2. MQTT Settings                                       │
│ 3. LoRa Settings                                       │
│ 4. Repeater Settings                                   │
│ 5. Show Current Configuration                          │
│ 6. Save Configuration                                  │
│ 7. Reset to Defaults                                   │
│ 8. Restart Device                                      │
│ 0. Exit Configuration                                  │
└────────────────────────────────────────────────────────┘
```

### Step 2: Configure WiFi (Option 1)

```
Enter choice: 1

Enable WiFi (y/n) [n]: y
WiFi SSID []: YourWiFiName
WiFi Password []: YourPassword

✓ WiFi configuration updated
```

### Step 3: Configure MQTT (Option 2)

```
Enter choice: 2

Enable MQTT (y/n) [n]: y
MQTT Server [mqtt.example.com]: mqtt.yourserver.com
MQTT Port [1883]: 1883
MQTT Username []: (leave empty or enter username)
MQTT Password []: (leave empty or enter password)
Client ID [meshcore_gateway]: gateway_001
Topic Prefix [meshcore]: meshcore

Publish raw packets (y/n) [y]: y
Publish decoded messages (y/n) [y]: y
Subscribe to commands (y/n) [y]: y

✓ MQTT configuration updated
```

### Step 4: Save and Restart

```
Enter choice: 6
Saving configuration... ✓ Done!

Enter choice: 8
Restarting...
```

## 📡 Testing LoRa Reception

Once running, any LoRa packets received will show:

```
📡 LoRa RX: 15 bytes | RSSI: -85 dBm | SNR: 8.5 dB
   Data: 48 65 6C 6C 6F 20 4D 65 73 68 43 6F 72 65 21 
   Text: "Hello MeshCore!"
```

## 📊 Testing MQTT

### Subscribe to Gateway Messages

```bash
# On your computer, subscribe to all topics:
mosquitto_sub -h mqtt.yourserver.com -t "MESHCORE/#" -v

# You'll see:
MESHCORE/gateway/gateway_001/status {"online":true,"timestamp":12345,...}
MESHCORE/raw {"timestamp":12345,"rssi":-85,"snr":8.5,"data":"48656C6C6F..."}
MESHCORE/messages {"from":305419896,"to":4294967295,"message":"Hello MeshCore!",...}
```

### Send LoRa Packet from MQTT

```bash
# Send a message to LoRa network:
mosquitto_pub -h mqtt.yourserver.com \
  -t "MESHCORE/commands/send" \
  -m "Hello from MQTT!"
```

The gateway will transmit this via LoRa!

## 🎮 Serial Commands

| Key | Action |
|-----|--------|
| `c` | Configuration menu |
| `s` | Show statistics |
| `r` | Restart device |

### Statistics Example

Press `s` to see:

```
┌── Gateway Statistics ──────────────────────────────────┐
│ Uptime:           3600                                 │
│ Packets Received: 125                                  │
│ Packets Sent:     12                                   │
│ Packets Forwarded:115                                  │
│ Packets Failed:   2                                    │
│ Free Heap:        156000                               │
│ WiFi RSSI:        -65                                  │
│ IP Address:       192.168.1.100                        │
│ MQTT:             Connected                            │
└────────────────────────────────────────────────────────┘
```

## 🔧 Default Settings (Before Configuration)

| Setting | Default Value |
|---------|---------------|
| **LoRa** |
| Frequency | 915.0 MHz (US) |
| Bandwidth | 125.0 kHz |
| Spreading Factor | 7 |
| TX Power | 20 dBm |
| Sync Word | 0x12 |
| **WiFi** |
| Enabled | No |
| **MQTT** |
| Enabled | No |
| **Repeater** |
| Max Hops | 3 |
| Auto Repeat | Yes |

## ⚠️ Important Notes

### Antenna

**ALWAYS connect antenna before powering on!**
- Transmitting without antenna damages the LoRa module
- Use 915 MHz antenna for US, 868 MHz for EU

### Frequency

Change frequency based on your region:
- **US**: 915 MHz (default)
- **EU**: 868 MHz
- **Asia**: 433 MHz

To change: Press `c` → option `3` → change frequency → save → restart

### WiFi

- ESP32 only supports **2.4 GHz** networks (not 5 GHz)
- WPA2 Personal supported
- Enterprise WiFi may not work

## 🧪 Quick Tests

### Test 1: LoRa Radio Works

```bash
# In serial monitor, you should see:
Initializing SX1268 radio... success!
✓ Radio listening for packets
```

✅ If you see this, LoRa is working!

### Test 2: WiFi Connects

After configuring WiFi and restarting:

```bash
Connecting to WiFi: YourWiFiName
.....
✓ WiFi connected
IP: 192.168.1.100
```

✅ WiFi is working!

### Test 3: MQTT Connects

```bash
Connecting to MQTT: mqtt.yourserver.com
✓ MQTT connected
Subscribed to: MESHCORE/commands/#
```

✅ MQTT is working!

### Test 4: Receive LoRa Packet

Use another LoRa device or MeshCore node nearby and send a test message.
You should see packets appear in serial monitor and MQTT!

## 🐛 Troubleshooting

### Radio initialization failed

**Causes:**
- Antenna not connected
- Wiring issue
- Wrong pins

**Solution:**
1. Check antenna is connected
2. Verify pins in `src/config.h` match your board
3. Check all SPI connections

### WiFi won't connect

**Causes:**
- Wrong password
- 5 GHz network
- Too far from router

**Solution:**
1. Double-check SSID and password
2. Ensure it's 2.4 GHz WiFi
3. Move closer to router

### MQTT won't connect

**Causes:**
- Wrong server address
- Firewall blocking port 1883
- Authentication required

**Solution:**
1. Test broker with: `mosquitto_pub -h your-broker -t test -m test`
2. Check firewall allows port 1883
3. Verify username/password if required

### No packets received

**Causes:**
- No other LoRa devices nearby
- Wrong frequency
- Sync word mismatch

**Solution:**
1. Make sure another LoRa device is transmitting
2. Verify frequency matches (915/868/433 MHz)
3. Check sync word is 0x12 (MeshCore default)

## 🎓 Next Steps

1. ✅ Flash firmware (you're doing this now!)
2. ⬜ Configure WiFi and MQTT
3. ⬜ Test with another LoRa device
4. ⬜ Monitor MQTT messages
5. ⬜ Send messages from MQTT to LoRa
6. ⬜ Deploy and enjoy!

## 📚 More Information

- **Full Docs**: [README.md](README.md)
- **Serial Commands**: [SERIAL_COMMANDS.md](SERIAL_COMMANDS.md)
- **LilyGo Guide**: [LILYGO_SETUP.md](LILYGO_SETUP.md)
- **MeshCore**: https://github.com/meshcore-dev/MeshCore

---

## 🚀 You're Ready!

**Your firmware is fully functional and ready to flash!**

Just click the **Upload** button and start meshing! 📡✨

