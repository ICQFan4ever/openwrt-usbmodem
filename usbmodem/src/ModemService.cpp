#include "ModemService.h"

#include "Modem/Asr1802.h"
#include "Uci.h"

#include <vector>
#include <signal.h>

ModemService::ModemService(const std::string &iface): m_iface(iface) {
	m_start_time = getCurrentTimestamp();
	
	// Default options
	m_uci_options["proto"] = "";
	m_uci_options["modem_device"] = "";
	m_uci_options["modem_speed"] = "115200";
	m_uci_options["modem_type"] = "";
	m_uci_options["auth_type"] = "";
	m_uci_options["pdp_type"] = "IP";
	m_uci_options["apn"] = "internet";
	m_uci_options["username"] = "";
	m_uci_options["password"] = "";
	m_uci_options["pincode"] = "";
	m_uci_options["prefer_dhcp"] = "0";
	m_uci_options["prefer_sms_to_sim"] = "0";
	m_uci_options["force_network_restart"] = "0";
	m_uci_options["connect_timeout"] = "300";
}

bool ModemService::validateOptions() {
	if (m_uci_options["proto"] != "usbmodem") {
		LOGE("Uunsupported protocol: %s\n", m_uci_options["proto"].c_str());
		return false;
	}
	
	if (!m_uci_options["modem_device"].size()) {
		LOGE("Please, specify `modem_device` in config.\n");
		return false;
	}
	
	if (!m_uci_options["modem_type"].size()) {
		LOGE("Please, specify `modem_type` in config.\n");
		return false;
	}
	
	if (m_uci_options["pdp_type"] != "IP" && m_uci_options["pdp_type"] != "IPV6" && m_uci_options["pdp_type"] != "IPV4V6") {
		LOGE("Invalid PDP type: %s\n", m_uci_options["pdp_type"].c_str());
		return false;
	}
	
	if (m_uci_options["auth_type"] != "" && m_uci_options["auth_type"] != "pap" && m_uci_options["auth_type"] != "chap") {
		LOGE("Invalid auth type: %s\n", m_uci_options["auth_type"].c_str());
		return false;
	}
	
	return true;
}

bool ModemService::startDhcp() {
	if (m_dhcp_inited) {
		if (m_uci_options["pdp_type"] == "IP" || m_uci_options["pdp_type"] == "IPV4V6") {
			if (!m_netifd.dhcpRenew(m_iface + "_4")) {
				LOGE("Can't send dhcp renew for '%s_4'\n", m_iface.c_str());
				return false;
			}
		}
		
		if (m_uci_options["pdp_type"] == "IPV6" || m_uci_options["pdp_type"] == "IPV4V6") {
			if (!m_netifd.dhcpRenew(m_iface + "_6")) {
				LOGE("Can't send dhcp renew for '%s_6'\n", m_iface.c_str());
				return false;
			}
		}
	} else {
		if (m_uci_options["pdp_type"] == "IP" || m_uci_options["pdp_type"] == "IPV4V6") {
			if (!m_netifd.createDynamicIface("dhcp", m_iface + "_4", m_iface, m_firewall_zone, m_uci_options)) {
				LOGE("Can't create dhcp interface '%s_4'\n", m_iface.c_str());
				return false;
			}
		}
		
		if (m_uci_options["pdp_type"] == "IPV6" || m_uci_options["pdp_type"] == "IPV4V6") {
			if (!m_netifd.createDynamicIface("dhcpv6", m_iface + "_4", m_iface, m_firewall_zone, m_uci_options)) {
				LOGE("Can't create dhcp interface '%s_4'\n", m_iface.c_str());
				return false;
			}
		}
		
		m_dhcp_inited = true;
	}
	return true;
}

bool ModemService::stopDhcp() {
	if (!m_dhcp_inited)
		return true;
	
	if (m_uci_options["pdp_type"] == "IP" || m_uci_options["pdp_type"] == "IPV4V6") {
		if (!m_netifd.dhcpRelease(m_iface + "_4")) {
			LOGE("Can't send dhcp release for '%s_4'\n", m_iface.c_str());
			return false;
		}
	}
	
	if (m_uci_options["pdp_type"] == "IPV6" || m_uci_options["pdp_type"] == "IPV4V6") {
		if (!m_netifd.dhcpRelease(m_iface + "_6")) {
			LOGE("Can't send dhcp release for '%s_6'\n", m_iface.c_str());
			return false;
		}
	}
	
	return true;
}

bool ModemService::init() {
	if (!Loop::init()) {
		LOGE("Can't init eventloop...\n");
		return setError("INTERNAL_ERROR", true);
	}
	
	if (!m_ubus.open()) {
		LOGE("Can't init ubus...\n");
		return setError("INTERNAL_ERROR", true);
	}
	
	m_netifd.setUbus(&m_ubus);
	
	if (!Uci::loadIfaceConfig(m_iface, &m_uci_options)) {
		LOGE("Can't read config for interface: %s\n", m_iface.c_str());
		return setError("INVALID_CONFIG", true);
	}
	
	if (!validateOptions())
		return setError("INVALID_CONFIG", true);
	
	if (!Uci::loadIfaceFwZone(m_iface, &m_firewall_zone)) {
		LOGE("Can't find fw3 zone for interface: %s\n", m_iface.c_str());
		return setError("INVALID_CONFIG", true);
	}
	
	m_tty_speed = strToInt(m_uci_options["modem_speed"]);
	m_tty_path = findTTY(m_uci_options["modem_device"]);
	m_net_iface = findNetByTTY(m_tty_path);
	
	if (!m_tty_path.size()) {
		LOGE("Device not found: %s\n", m_uci_options["modem_device"].c_str());
		return setError("NO_DEVICE");
	}
	
	if (hasNetDev()) {
		if (!m_net_iface.size()) {
			LOGE("Network device not found: %s\n", m_uci_options["modem_device"].c_str());
			return setError("NO_DEVICE");
		}
	}
	
	// Link modem net dev to interface
	if (!m_netifd.updateIface(m_iface, m_net_iface, nullptr, nullptr)) {
		LOGE("Can't init iface...\n");
		return setError("INTERNAL_ERROR");
	}
	
	return true;
}

bool ModemService::runModem() {
	// Get modem driver
	if (m_uci_options["modem_type"] == "asr1802") {
		m_modem = new ModemAsr1802();
	} else {
		LOGE("Unsupported modem type: %s\n", m_uci_options["modem_type"].c_str());
		return setError("INVALID_CONFIG", true);
	}
	
	// Setup main options to driver
	m_modem->setPdpConfig(m_uci_options["pdp_type"], m_uci_options["apn"], m_uci_options["auth_type"], m_uci_options["username"], m_uci_options["password"]);
	m_modem->setPinCode(m_uci_options["pincode"]);
	m_modem->setSerial(m_tty_path, m_tty_speed);
	
	// Setup custom options to driver
	m_modem->setCustomOption<bool>("prefer_dhcp", m_uci_options["prefer_dhcp"] == "1");
	m_modem->setCustomOption<bool>("prefer_sms_to_sim", m_uci_options["prefer_sms_to_sim"] == "1");
	m_modem->setCustomOption<int>("connect_timeout", strToInt(m_uci_options["connect_timeout"]) * 1000);
	
	m_modem->on<Modem::EvNetworkChanged>([=](const auto &event) {
		if (event.status == Modem::NET_NOT_REGISTERED) {
			LOGD("Unregistered from network\n");
		} else if (event.status == Modem::NET_SEARCHING) {
			LOGD("Searching network...\n");
		} else {
			LOGD("Registered to %s network\n", Modem::getNetRegStatusName(event.status));
		}
	});
	
	m_modem->on<Modem::EvTechChanged>([=](const auto &event) {
		if (event.tech == Modem::TECH_NO_SERVICE || event.tech == Modem::TECH_UNKNOWN) {
			LOGD("Network mode: none\n");
		} else {
			LOGD("Network mode: %s\n", Modem::getTechName(event.tech));
		}
	});
	
	m_modem->on<Modem::EvDataConnected>([=](const auto &event) {
		int dhcp_delay = 0;
		if (event.is_update) {
			int diff = getCurrentTimestamp() - m_last_connected;
			m_last_connected = getCurrentTimestamp();
			dhcp_delay = m_modem->getDelayAfterDhcpRelease();
			LOGD("Internet connection changed, last session %d ms\n", diff);
		} else {
			m_last_connected = getCurrentTimestamp();
			if (m_last_disconnected) {
				int diff = m_last_connected - m_last_disconnected;
				dhcp_delay = std::max(0, m_modem->getDelayAfterDhcpRelease() - diff);
				LOGD("Internet connected, downtime %d ms\n", diff);
			} else {
				int diff = m_last_connected - m_start_time;
				LOGD("Internet connected, init time %d ms\n", diff);
			}
		}
		
		Modem::IpInfo ipv4 = m_modem->getIpInfo(4);
		Modem::IpInfo ipv6 = m_modem->getIpInfo(6);
		
		if (m_modem->getIfaceProto() == Modem::IFACE_STATIC) {
			if (!m_netifd.updateIface(m_iface, m_net_iface, &ipv4, &ipv6)) {
				LOGE("Can't set IP to interface '%s'\n", m_iface.c_str());
				setError("INTERNAL_ERROR");
			}
		} else if (m_modem->getIfaceProto() == Modem::IFACE_DHCP) {
			if (dhcp_delay > 0) {
				LOGD("Wait %d ms for DHCP recovery...\n", dhcp_delay);
				Loop::setTimeout([=]() {
					if (!startDhcp())
						setError("INTERNAL_ERROR");
				}, dhcp_delay);
			} else {
				if (!startDhcp())
					setError("INTERNAL_ERROR");
			}
		}
		
		if (ipv4.ip.size() > 0) {
			LOGD(
				"-> IPv4: ip=%s, gw=%s, mask=%s, dns1=%s, dns2=%s\n",
				ipv4.ip.c_str(), ipv4.gw.c_str(), ipv4.mask.c_str(), ipv4.dns1.c_str(), ipv4.dns2.c_str()
			);
		}
		
		if (ipv6.ip.size() > 0) {
			LOGD(
				"-> IPv6: ip=%s, gw=%s, mask=%s, dns1=%s, dns2=%s\n",
				ipv6.ip.c_str(), ipv6.gw.c_str(), ipv6.mask.c_str(), ipv6.dns1.c_str(), ipv6.dns2.c_str()
			);
		}
	});
	
	m_modem->on<Modem::EvDataConnecting>([=](const auto &event) {
		LOGD("Connecting to internet...\n");
	});
	
	m_modem->on<Modem::EvDataDisconnected>([=](const auto &event) {
		m_last_disconnected = getCurrentTimestamp();
		int diff = m_last_disconnected - m_last_connected;
		LOGD("Internet disconnected, last session %d ms\n", diff);
		
		if (m_modem->getIfaceProto() == Modem::IFACE_STATIC) {
			if (!m_netifd.updateIface(m_iface, m_net_iface, nullptr, nullptr)) {
				LOGE("Can't set IP to interface '%s'\n", m_iface.c_str());
				setError("INTERNAL_ERROR");
			}
		} else if (m_modem->getIfaceProto() == Modem::IFACE_DHCP) {
			if (!stopDhcp()) {
				setError("INTERNAL_ERROR");
			}
		}
	});
	
	m_modem->on<Modem::EvSignalLevels>([=](const auto &event) {
		Modem::SignalLevels levels = m_modem->getSignalLevels();
		
		std::vector<std::string> info;
		
		if (!std::isnan(levels.rssi_dbm))
			info.push_back("RSSI: " + numberFormat(levels.rssi_dbm, 1) + " dBm");
		
		if (!std::isnan(levels.bit_err_pct))
			info.push_back("Bit errors: " + numberFormat(levels.bit_err_pct, 1) + "%");
		
		if (!std::isnan(levels.rscp_dbm))
			info.push_back("RSCP: " + numberFormat(levels.rscp_dbm, 1) + " dBm");
		
		if (!std::isnan(levels.eclo_db))
			info.push_back("Ec/lo: " + numberFormat(levels.eclo_db, 1) + " dB");
		
		if (!std::isnan(levels.rsrq_db))
			info.push_back("RSRQ: " + numberFormat(levels.rsrq_db, 1) + " dB");
		
		if (!std::isnan(levels.rsrp_dbm))
			info.push_back("RSRP: " + numberFormat(levels.rsrp_dbm, 1) + " dBm");
		
		if (info.size() > 0) {
			std::string str = strJoin(info, ", ");
			LOGD("%s\n", str.c_str());
		}
	});
	
	m_modem->on<Modem::EvPinStateChaned>([=](const auto &event) {
		if (event.state == Modem::PIN_ERROR) {
			LOGD("PIN: invalid code, or required PIN2, PUK, PUK2 or other.\n");
		} else if (event.state == Modem::PIN_READY) {
			LOGD("PIN: success\n");
		} else if (event.state == Modem::PIN_REQUIRED) {
			LOGD("PIN: need unlock\n");
		} else if (event.state == Modem::PIN_NOT_SUPPORTED) {
			LOGD("PIN: not supported\n");
		}
	});
	
	m_modem->on<Modem::EvIoBroken>([=](const auto &event) {
		LOGE("TTY device is lost...\n");
		setError("IO_ERROR");
	});
	
	m_modem->on<Modem::EvDataConnectTimeout>([=](const auto &event) {
		LOGE("Internet connection timeout...\n");
		setError("CONNECT_TIMEOUT");
	});
	
	if (!m_modem->open()) {
		LOGE("Can't initialize modem.\n");
		return setError("INIT_ERROR");
	}
	
	LOGD("Modem: %s %s\n", m_modem->getVendor().c_str(), m_modem->getModel().c_str());
	LOGD("IMEI: %s\n", m_modem->getImei().c_str());
	
	return true;
}

void ModemService::finishModem() {
	if (m_modem) {
		m_modem->finish();
		m_modem->close();
		m_modem = nullptr;
		delete m_modem;
	}
}

bool ModemService::setError(const std::string &code, bool fatal) {
	m_error_code = code;
	m_error_fatal = fatal;
	
	Loop::stop();
	
	return false;
}

int ModemService::checkError() {
	if (!m_error_code.size())
		return 0;
	
	// User callback
	execFile("/etc/usbmodem.user", {}, {
		"action=error",
		"error=" + m_error_code,
		"is_fatal_error=" + std::string(m_error_fatal ? "1" : "0"),
		"interface=" + m_iface
	});
	
	if (!m_netifd.avail()) {
		LOGD("%s: %s\n", (m_error_fatal ? "Fatal" : "Error"), m_error_code.c_str());
		LOGD("Can't send error to netifd, because it not inited...\n");
		sleep(5);
		return 1;
	}
	
	if (m_error_code == "NO_DEVICE") {
		if (!m_netifd.protoSetAvail(m_iface, false)) {
			LOGE("Can't send available=false to netifd...\n");
			sleep(5);
		}
	} else {
		if (m_error_fatal) {
			if (!m_netifd.protoBlockRestart(m_iface)) {
				LOGE("Can't send restart blocking '%s' to netifd...\n", m_error_code.c_str());
				sleep(5);
			}
			
			if (!m_netifd.protoError(m_iface, m_error_code))
				LOGE("Can't send error '%s' to netifd...\n", m_error_code.c_str());
		} else {
			sleep(5);
			
			if (!m_netifd.protoError(m_iface, m_error_code))
				LOGE("Can't send error '%s' to netifd...\n", m_error_code.c_str());
		}
	}
	
	return 1;
}

int ModemService::run() {
	if (init()) {
		if (runModem()) {
			Loop::setTimeout([=]() {
				if (!runApi())
					LOGE("Can't start API server, but continuing running...\n");
			}, 0);
			Loop::run();
		}
		finishModem();
	}
	
	int diff = getCurrentTimestamp() - m_start_time;
	LOGD("Done, total uptime: %d ms\n", diff);
	
	return checkError();
}
