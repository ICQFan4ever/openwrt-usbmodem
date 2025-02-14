#include "Asr1802.h"
#include "../Loop.h"

ModemAsr1802::ModemAsr1802() : ModemBaseAt() {
	
}

/*
 * Custom options
 * */
bool ModemAsr1802::setCustomOption(const std::string &name, const std::any &value) {
	if (ModemBaseAt::setCustomOption(name, value))
		return true;
	
	if (name == "force_restart_network") {
		m_force_restart_network = std::any_cast<bool>(value);
		return true;
	} else if (name == "prefer_dhcp") {
		m_prefer_dhcp = std::any_cast<bool>(value);
		return true;
	}
	
	return false;
}

bool ModemAsr1802::initDefaults() {
	const char *init_commands[] = {
		// Enable extended error codes
		"AT+CMEE=1",
		
		// Enable all network registration unsolicited events
		"AT+CREG=2",
		"AT+CGREG=2",
		"AT+CEREG=2",
		
		// Enable CGEV events
		"AT+CGEREP=2,0",
		
		// Setup indication mode of new message to TE
		"AT+CNMI=0,1,0,2,0",
		
		// Enable network indicators unsolicited events
		"AT+CIND=1",
		
		// USSD mode
		"AT+CUSD=1",
		
		// Enable background search
		"AT+BGLTEPLMN=1,30",
		
		nullptr
	};
	
	const char **cursor = init_commands;
	while (*cursor) {
		if (m_at.sendCommandNoResponse(*cursor) != 0) {
			LOGE("AT cmd failed: %s\n", *cursor);
			return false;
		}
		cursor++;
	}
	
	return true;
}

void ModemAsr1802::handleCgev(const std::string &event) {
	// "DEACT" and "DETACH" mean disconnect
	if (event.find("DEACT") != std::string::npos || event.find("DETACH") != std::string::npos) {
		Loop::setTimeout([=]() {
			handleDisconnect();
		}, 0);
	}
	// Other events handle as "connection changed"
	else {
		// Ignore this event for 3G/EDGE
		if (m_tech == TECH_LTE) {
			Loop::setTimeout([=]() {
				handleConnect();
			}, 0);
		}
	}
}

void ModemAsr1802::handleCreg(const std::string &event) {
	Creg *reg = nullptr;
	
	if (strStartsWith(event, "+CREG")) {
		reg = &m_creg;
	} else if (strStartsWith(event, "+CGREG")) {
		reg = &m_cgreg;
	} else if (strStartsWith(event, "+CEREG")) {
		reg = &m_cereg;
	} else {
		// Invalid data
		return;
	}
	
	uint32_t cell_id = 0, loc_id = 0;
	int tech = CREG_TECH_UNKNOWN, stat = CREG_NOT_REGISTERED;
	bool parsed = false;
	
	switch (AtParser::getArgCnt(event)) {
		/* +CREG: <stat> */
		/* +CGREG: <stat> */
		/* +CEREG: <stat> */
		case 1:
			parsed = AtParser(event)
				.parseInt(&stat)
				.success();
		break;
		
		/* +CREG: <n>, <stat> */
		/* +CGREG: <n>, <stat> */
		/* +CEREG: <n>, <stat> */
		case 2:
			parsed = AtParser(event)
				.parseSkip()
				.parseInt(&stat)
				.success();
		break;
		
		/* +CREG: <state>, <lac>, <cid>, <act> */
		/* +CEREG: <state>, <lac>, <cid>, <act> */
		case 4:
			parsed = AtParser(event)
				.parseInt(&stat)
				.parseUInt(&loc_id, 16)
				.parseUInt(&cell_id, 16)
				.parseInt(&tech)
				.success();
		break;
		
		/* +CREG: <n>, <state>, <lac>, <cid>, <act> */
		/* +CEREG: <n>, <state>, <lac>, <cid>, <act> */
		/* +CGREG: <state>, <lac>, <cid>, <act>, <rac> */
		case 5:
			if (strStartsWith(event, "+CGREG")) {
				parsed = AtParser(event)
					.parseInt(&stat)
					.parseUInt(&loc_id, 16)
					.parseUInt(&cell_id, 16)
					.parseInt(&tech)
					.parseSkip()
					.success();
			} else {
				parsed = AtParser(event)
					.parseSkip()
					.parseInt(&stat)
					.parseUInt(&loc_id, 16)
					.parseUInt(&cell_id, 16)
					.parseInt(&tech)
					.success();
			}
		break;
		
		/* +CGREG: <n>, <state>, <lac>, <cid>, <act>, <rac> */
		case 6:
			parsed = AtParser(event)
				.parseSkip()
				.parseInt(&stat)
				.parseUInt(&loc_id, 16)
				.parseUInt(&cell_id, 16)
				.parseInt(&tech)
				.parseSkip()
				.success();
		break;
	}
	
	if (!parsed) {
		LOGE("Invalid CREG: %s\n", event.c_str());
		return;
	}
	
	reg->status = static_cast<CregStatus>(stat);
	reg->tech = static_cast<CregTech>(tech);
	reg->loc_id = loc_id & 0xFFFF;
	reg->cell_id = cell_id & 0xFFFF;
	
	handleNetworkChange();
}

void ModemAsr1802::handleUssdResponse(int code, const std::string &data, int dcs) {
	// Fix broken USSD dcs
	if (dcs == 17)
		dcs = 72; /* UCS2 */
	
	if (dcs == 0)
		dcs = 68; /* 8bit GSM */
	
	ModemBaseAt::handleUssdResponse(code, data, dcs);
}

void ModemAsr1802::handleCesq(const std::string &event) {
	ModemBaseAt::handleCesq(event);
	
	if (std::isnan(m_levels.rssi_dbm) && !std::isnan(m_levels.rscp_dbm))
		m_levels.rssi_dbm = m_levels.rscp_dbm;
}

bool ModemAsr1802::dial() {
	std::string cmd;
	
	int auth_type = 0;
	if (m_pdp_auth_mode == "pap")
		auth_type = 1;
	if (m_pdp_auth_mode == "chap")
		auth_type = 2;
	
	// Configure PDP context
	cmd = "AT+CGDCONT=" + std::to_string(m_pdp_context) + ",\"" + m_pdp_type + "\",\"" + m_pdp_apn + "\"";
	if (m_at.sendCommandNoResponse(cmd) != 0)
		return false;
	
	// Set PPP auth
	cmd = "AT*AUTHReq=" + std::to_string(m_pdp_context) + "," + std::to_string(auth_type) + ",\"" + m_pdp_user + "\",\"" + m_pdp_password + "\"";
	if (m_at.sendCommandNoResponse(cmd) != 0)
		return false;
	
	// Start dialing
	cmd = "AT+CGDATA=\"\"," + std::to_string(m_pdp_context);
	auto response = m_at.sendCommandDial(cmd);
	if (response.error) {
		LOGD("Dial error: %s\n", response.status.c_str());
		return false;
	}
	
	return true;
}

void ModemAsr1802::handleNetworkChange() {
	NetworkTech new_tech;
	NetworkReg new_net_reg;
	
	if (m_cereg.isRegistered()) {
		new_tech = m_cereg.toNetworkTech();
		new_net_reg = m_cereg.toNetworkReg();
	} else if (m_cgreg.isRegistered()) {
		new_tech = m_cgreg.toNetworkTech();
		new_net_reg = m_cgreg.toNetworkReg();
	} else {
		new_tech = TECH_NO_SERVICE;
		new_net_reg = m_creg.toNetworkReg();
	}
	
	if (m_net_reg != new_net_reg) {
		m_net_reg = new_net_reg;
		emit<EvNetworkChanged>({.status = m_net_reg});
	}
	
	if (m_tech != new_tech) {
		m_tech = new_tech;
		emit<EvTechChanged>({.tech = m_tech});
	}
}

int ModemAsr1802::getCurrentPdpCid() {
	auto response = m_at.sendCommand("AT+CGCONTRDP=?", "+CGCONTRDP");
	if (response.error)
		return -1;
	
	if (!response.data().size())
		return 0;
	
	int cid;
	if (!AtParser(response.data()).parseInt(&cid).success())
		return -1;
	
	return cid;
}

void ModemAsr1802::handleConnect() {
	std::string addr, gw, mask, dns1, dns2;
	
	// Without this command not works...
	if (m_at.sendCommandNoResponse("AT+CGDCONT?") != 0) {
		handleConnectError();
		return;
	}
	
	// Get current PDP context id
	int cid = getCurrentPdpCid();
	if (cid < 0) {
		handleConnectError();
		return;
	}
	
	// Get PDP context info
	auto response = m_at.sendCommand("AT+CGCONTRDP=" + std::to_string(cid), "+CGCONTRDP");
	if (response.error || !response.lines.size()) {
		handleConnectError();
		return;
	}
	
	for (auto &line: response.lines) {
		int arg_cnt = AtParser::getArgCnt(line);
		
		AtParser parser(line);
		
		// <cid>, <bearer_id>, <apn>
		parser.parseSkip().parseSkip().parseSkip();
		
		// <local_addr>
		if (arg_cnt >= 4)
			parser.parseString(&addr);
		
		// <subnet_mask>
		if (arg_cnt >= 5)
			parser.parseString(&mask);
		
		// <gw_addr>
		if (arg_cnt >= 6)
			parser.parseString(&gw);
		
		// <DNS_prim_addr>
		if (arg_cnt >= 7)
			parser.parseString(&dns1);
		
		// <DNS_prim_addr>
		if (arg_cnt >= 8)
			parser.parseString(&dns2);
		
		if (!parser.success()) {
			handleConnectError();
			return;
		}
		
		int ipv = getIpType(addr, true);
		if (!ipv || !normalizeIp(&addr, ipv, true)) {
			LOGE("Invalid local IP: %s\n", addr.c_str());
			handleConnectError();
			return;
		}
		
		if (mask.size() > 0 && !normalizeIp(&mask, ipv, true)) {
			LOGE("Invalid subnet mask IP: %s\n", mask.c_str());
			handleConnectError();
			return;
		}
		
		if (gw.size() > 0 && !normalizeIp(&gw, ipv, true)) {
			LOGE("Invalid gw IP: %s\n", gw.c_str());
			handleConnectError();
			return;
		}
		
		if (dns1.size() > 0 && !normalizeIp(&dns1, ipv, true)) {
			LOGE("Invalid dns1 IP: %s\n", dns1.c_str());
			handleConnectError();
			return;
		}
		
		if (dns2.size() > 0 && !normalizeIp(&dns2, ipv, true)) {
			LOGE("Invalid dns2 IP: %s\n", dns2.c_str());
			handleConnectError();
			return;
		}
		
		if (ipv == 6) {
			m_ipv6.ip = addr;
			m_ipv6.gw = gw;
			m_ipv6.mask = mask;
			m_ipv6.dns1 = dns1;
			m_ipv6.dns2 = dns2;
		} else {
			m_ipv4.ip = addr;
			m_ipv4.gw = gw;
			m_ipv4.mask = mask;
			m_ipv4.dns1 = dns1;
			m_ipv4.dns2 = dns2;
		}
	}
	
	bool is_update = m_data_state == CONNECTED;
	
	m_data_state = CONNECTED;
	m_connect_errors = 0;
	
	emit<EvDataConnected>({
		.is_update = is_update,
	});
}

void ModemAsr1802::handleConnectError() {
	LOGE("Can't get PDP context info...\n");
	handleDisconnect();
	restartNetwork();
}

void ModemAsr1802::handleDisconnect() {
	m_ipv4.gw = "";
	m_ipv4.ip = "";
	m_ipv4.mask = "";
	m_ipv4.dns1 = "";
	m_ipv4.dns2 = "";
	
	m_ipv6.gw = "";
	m_ipv6.ip = "";
	m_ipv6.mask = "";
	m_ipv6.dns1 = "";
	m_ipv6.dns2 = "";
	
	if (m_data_state == CONNECTED) {
		m_data_state = DISCONNECTED;
		emit<EvDataDisconnected>({});
	} else {
		m_data_state = DISCONNECTED;
	}
}

void ModemAsr1802::startDataConnection() {
	// Manual connection to internet needed only for 3G/EDGE
	// On LTE modem connects to internet autmatically
	// And also we don't try connect if no service
	if (m_tech == TECH_LTE || m_tech == TECH_NO_SERVICE || m_tech == TECH_UNKNOWN)
		return;
	
	// Connection already scheduled
	if (m_manual_connect_timeout != -1)
		return;
	
	// Already connected or connecting
	if (m_data_state != DISCONNECTED)
		return;
	
	m_data_state = CONNECTING;
	emit<EvDataConnecting>({});
	
	Loop::setTimeout([this]() {
		if (dial()) {
			handleConnect();
		} else {
			m_connect_errors++;
			handleDisconnect();
			
			if (m_connect_errors > 10) {
				m_connect_errors = 0;
				
				LOGD("Trying restart modem network by entering to flight mode...\n");
				restartNetwork();
			}
			
			// Try reconnect after few seconds
			m_manual_connect_timeout = Loop::setTimeout([=]() {
				m_manual_connect_timeout = -1;
				startDataConnection();
			}, 1000);
		}
	}, 0);
}

void ModemAsr1802::restartNetwork() {
	Loop::setTimeout([=]() {
		setRadioOn(false);
		
		Loop::setTimeout([=]() {
			setRadioOn(true);
		}, 0);
	}, 0);
}

bool ModemAsr1802::syncApn() {
	AtParser parser;
	std::string cmd, old_pdp_type, old_pdp_apn, old_user, old_password;
	int old_etif = 0, arg_cnt, old_auth_type;
	AtChannel::Response response;
	
	int auth_type = 0;
	if (m_pdp_auth_mode == "pap")
		auth_type = 1;
	if (m_pdp_auth_mode == "chap")
		auth_type = 2;
	
	// Get LTE pdp config
	response = m_at.sendCommand("AT*CGDFLT=1", "*CGDFLT");
	if (response.error)
		return false;
	
	arg_cnt = AtParser::getArgCnt(response.data());
	
	// <PDP_type>
	parser.parse(response.data()).parseString(&old_pdp_type);
	
	// <APN>
	if (arg_cnt >= 2)
		parser.parseString(&old_pdp_apn);
	
	// <etif> (???)
	if (arg_cnt >= 20) {
		parser
			.parseSkip().parseSkip().parseSkip().parseSkip()
			.parseSkip().parseSkip().parseSkip().parseSkip()
			.parseSkip().parseSkip().parseSkip().parseSkip()
			.parseSkip().parseSkip().parseSkip().parseSkip()
			.parseSkip().parseInt(&old_etif);
	}
	
	if (!parser.success())
		return false;
	
	// Get LTE pdp auth
	response = m_at.sendCommand("AT*CGDFAUTH?", "*CGDFAUTH");
	if (response.error)
		return false;
	
	// <auth_type> <username> <password>
	parser
		.parse(response.data())
		.parseInt(&old_auth_type)
		.parseString(&old_user)
		.parseString(&old_password);
	
	if (!parser.success())
		return false;
	
	bool need_change = (
		m_pdp_type != old_pdp_type || m_pdp_apn != old_pdp_apn || old_etif != 1 ||
		m_pdp_user != old_user || m_pdp_password != old_password || auth_type != old_auth_type
	);
	
	if (need_change) {
		bool radio_on = isRadioOn();
		
		if (radio_on && !setRadioOn(false))
			return false;
		
		cmd = "AT*CGDFLT=1,\"" + m_pdp_type + "\",\"" + m_pdp_apn + "\",,,,,,,,,,,,,,,,,,1";
		if (m_at.sendCommandNoResponse(cmd) != 0)
			return false;
		
		cmd = "AT*CGDFAUTH=1," + std::to_string(auth_type) + ",\"" + m_pdp_user + "\",\"" + m_pdp_password + "\"";
		if (m_at.sendCommandNoResponse(cmd) != 0)
			return false;
		
		if (radio_on && !setRadioOn(true))
			return false;
	}
	
	return true;
}

bool ModemAsr1802::isRadioOn() {
	auto response = m_at.sendCommand("AT+CFUN?", "+CFUN");
	if (response.error)
		return false;
	
	int cfun_state;
	if (!AtParser(response.data()).parseNextInt(&cfun_state))
		return false;
	
	return cfun_state == 1;
}

bool ModemAsr1802::setRadioOn(bool state) {
	std::string cmd = "AT+CFUN=" + std::to_string(state ? 1 : 4);
	return m_at.sendCommandNoResponse(cmd) == 0;
}

bool ModemAsr1802::init() {
	// Default AT timeout for this modem
	m_at.setDefaultTimeout(10 * 1000);
	
	// Currently is a fastest way for get internet after "cold" boot when using DCHP
	// Without this DHCP not respond up to 20 sec
	if (getIfaceProto() == IFACE_DHCP)
		m_force_restart_network = true;
	
	if (m_force_restart_network) {
		// Poweroff radio for reset any PDP contexts
		if (!setRadioOn(false))
			return false;
	}
	
	if (!initDefaults())
		return false;
	
	if (!readModemIdentification())
		return false;
	
	if (!syncApn())
		return false;
	
	// Network registration
	auto network_creg_handler = [=](const std::string &event) {
		handleCreg(event);
	};
	m_at.onUnsolicited("+CREG", network_creg_handler);
	m_at.onUnsolicited("+CGREG", network_creg_handler);
	m_at.onUnsolicited("+CEREG", network_creg_handler);
	
	// USSD response
	m_at.onUnsolicited("+CUSD", [=](const std::string &event) {
		handleCusd(event);
	});
	
	// PDP context events
	m_at.onUnsolicited("+CGEV", [=](const std::string &event) {
		handleCgev(event);
	});
	
	// Signal levels
	m_at.onUnsolicited("+CESQ", [=](const std::string &event) {
		handleCesq(event);
	});
	
	// SIM pin state
	m_at.onUnsolicited("+CPIN", [=](const std::string &event) {
		handleCpin(event);
	});
	
	if (!m_force_restart_network) {
		Loop::setTimeout([=]() {
			// Detect, if already have internet
			if (m_data_state == DISCONNECTED) {
				int cid = getCurrentPdpCid();
				if (cid > 0) {
					handleConnect();
				} else if (cid < 0) {
					restartNetwork();
				}
			}
			
			// Sync state
			m_at.sendCommandNoResponse("AT+CREG?");
			m_at.sendCommandNoResponse("AT+CGREG?");
			m_at.sendCommandNoResponse("AT+CEREG?");
			m_at.sendCommandNoResponse("AT+CESQ");
		}, 0);
	}
	
	Loop::setTimeout([=]() {
		if (!intiSms())
			LOGD("SMS not supported by this modem.\n");
	}, 0);
	
	// Sync SIM state
	startSimPolling();
	
	// Handle disconnects
	on<EvDataDisconnected>([=](const auto &event) {
		startDataConnection();
		startNetRegWhatchdog();
	});
	
	on<EvDataConnected>([=](const auto &event) {
		stopNetRegWhatchdog();
	});
	
	on<EvTechChanged>([=](const auto &event) {
		startDataConnection();
		stopNetRegWhatchdog();
	});
	
	on<EvPinStateChaned>([=](const auto &event) {
		if (event.state == PIN_READY || event.state == PIN_NOT_SUPPORTED)
			readSimIdentification();
	});
	
	// Poweron
	if (!setRadioOn(true))
		return false;
	
	if (m_data_state != CONNECTED)
		startNetRegWhatchdog();
	
	return true;
}

// At this function we have <5s, otherwise netifd send SIGKILL
void ModemAsr1802::finish() {
	// Disable unsolicited for prevent side effects
	m_at.resetUnsolicitedHandlers();
	
	// Poweroff radio
	m_at.sendCommandNoResponse("AT+CFUN=4", 5000);
}

ModemAsr1802::~ModemAsr1802() {
	
}

/*
 * Modem customizations
 * */
int ModemAsr1802::getDelayAfterDhcpRelease() {
	return 2000;
}

ModemAsr1802::IfaceProto ModemAsr1802::getIfaceProto() {
	// Modem supports two different protocols
	if (m_prefer_dhcp)
		return IFACE_DHCP;
	return IFACE_STATIC;
}

int ModemAsr1802::getCommandTimeout(const std::string &cmd) {
	if (strStartsWith(cmd, "AT+CFUN"))
		return 50 * 1000;
	
	if (strStartsWith(cmd, "AT+CGDATA"))
		return 185 * 1000;
	
	if (strStartsWith(cmd, "AT+CUSD"))
		return 110 * 1000;
	
	if (strStartsWith(cmd, "AT+CGATT"))
		return 110 * 1000;
	
	if (strStartsWith(cmd, "AT+CPIN"))
		return 185 * 1000;
	
	return ModemBaseAt::getCommandTimeout(cmd);
}
