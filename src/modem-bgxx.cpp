#include "modem-bgxx.hpp"

Modem op = {
	/* radio */ 		0
	/* cops */ 		 0,
	/* force */false,
};

TCP tcp[MAX_CONNECTIONS];
MQTT mqtt[MAX_MQTT_CONNECTIONS];

State st = {
	/* ready */							false,
	/* gsm_ready */					false,
	/* apn_ready */					false,
	/* apn_connected */			false,
	/* did_config */   			false,
};

int8_t technology = -1; // delete
String tech = "";

int8_t mqtt_buffer[] = {-1,-1,-1,-1,-1}; // index of msg to read
bool (*parseMQTTmessage)(String,String);

uint32_t next_retry = 0;
uint32_t clock_sync_timeout = 0;

void MODEMBGXX::init_port(uint32_t baudrate, uint32_t serial_config) {

	//modem->begin(baudrate);
	modem->begin(baudrate,serial_config,16,17);
	#ifdef DEBUG_BG95
	log("modem bus inited");
	#endif
	while (modem->available()) {
		String command = modem->readStringUntil(AT_TERMINATOR);

		command.trim();

		if (command.length() > 0) {
			#ifdef DEBUG_BG95
			log("[init port] ignoring '" + command + "'");
			#endif
		}
	}
}

void MODEMBGXX::disable_port() {
	modem->end();
}

bool MODEMBGXX::init(uint8_t radio, uint8_t cops, uint8_t pwkey){

	op->pwkey = pwkey;
	pinMode(pwkey, OUTPUT);

	op->radio = radio;
	op->cops = cops;

	if(!powerCycle())
		return false;

	if(!config())
		return false;

	if(!configure_radio_mode(radio,cops))
		return false;

	return true;
}

/*
bool MODEMBGXX::gsm_ready() {
	return st.gsm_ready;
}

bool MODEMBGXX::apn_ready() {
	return st.apn_ready;
}
*/
bool MODEMBGXX::apn_connected(uint8_t contextID) {
	if(cid == 0 || cid > MAX_CONNECTIONS)
		return false;

	return apn[contextID-1].connected;
}

bool MODEMBGXX::has_context(uint8_t cid) {
	if(cid == 0 || cid > MAX_CONNECTIONS)
		return false;

	return connected_state[cid-1];
}

bool MODEMBGXX::ready() {

	if(ready_until > millis())
		return st.ready;

	ready_until = millis() + 2000;

	uint32_t t = millis() + 3000;

	while (t > millis()) {
		if (check_command("AT", "OK", "ERROR", 1000)) {
			st.ready = true;
			return st.ready;
		}
		delay(1000);
	}

	st.ready = false;
	return st.ready;
}

bool MODEMBGXX::wait_modem_to_init(){
	if(wait_command("RDY",10000)){
		reset();
		if(wait_command("APP RDY",10000)){
			increase_modem_reboot();
			return true;
		}
	}
	return false;
}

bool MODEMBGXX::switchOn(){

	#ifdef DEBUG_BG95
	log("switching modem");
	#endif
	digitalWrite(pwkey, HIGH);
	delay(2000);

	digitalWrite(pwkey, LOW);

	wait_modem_to_init();
}

void MODEMBGXX::increase_modem_reboot(){
	next_retry = millis() + 5*60*1000; // 15 minutes
}

bool MODEMBGXX::powerCycle(){

	#ifdef DEBUG_BG95
	log("power cycle modem");
	#endif
	digitalWrite(pwkey, HIGH);
	delay(2000);

	digitalWrite(pwkey, LOW);

	if(!wait_modem_to_init()){

		#ifdef DEBUG_BG95
		log("power cycle modem");
		#endif
		digitalWrite(pwkey, HIGH);
		delay(2000);

		digitalWrite(pwkey, LOW);

		return wait_modem_to_init();
	}

	return true;
}

bool MODEMBGXX::reset() {

	// APN reset
	st.apn_ready =false;

	st.gsm_ready = false;

	st.did_config = false;

	did_config = false;

	for (uint8_t i = 0; i < MAX_CONNECTIONS; i++) {
		data_pending[i]    = false;
		connected_state[i] = false;
	}
	for (uint8_t i = 0; i < MAX_TCP_CONNECTIONS; i++) {
		tcp_connected_state[i] = false;
	}
	for (uint8_t i = 0; i < MAX_MQTT_CONNECTIONS; i++) {
		mqtt_connected_state[i] = false;
	}

	return true;
}

bool MODEMBGXX::setup(uint8_t cid, String apn, String username, String password) {

	if(cid == 0 || cid > MAX_CONNECTIONS)
		return false;

	if (!did_config){
		config();
	}

	return check_command("AT+QICSGP="+String(cid)+",1,\"" + apn + "\",\"" + username + "\",\"" + password + "\"", "OK", "ERROR");  // replacing CGDCONT
}

bool MODEMBGXX::loop(uint32_t wait) {

	check_messages();

	check_data_pending();

	if(loop_until < millis()){
		get_rssi();

		get_state();

		MQTT_checkConnection();

		loop_until = millis()+wait;

		return true;
	}


	return false;
}

void MODEMBGXX::get_state() {

	String state = "";

	check_command("AT+CREG?","OK","ERROR",3000); // both

	if(apn_connected()){
		state = get_command("AT+QIACT?",15000);
		//log("response: "+state);
	}

	return;
}

int8_t MODEMBGXX::get_actual_mode(){
	return technology;
}

// deprecated
void MODEMBGXX::status() {
	return;
}

void MODEMBGXX::log_status() {

	//log("imei: "+get_imei());
	//log("ccid: "+get_ccid());
	//log("has context: "+String(has_context()));

	log("technology: "+tech);
	log("rssi: "+String(rssi()));

	for(uint8_t i = 0; i++; i<MAX_CONNECTIONS){
		if(connected_state[i])
			log(state[i].apn + " connected");
		else
			log(modem[i].apn + " disconnected");
	}
	return;
}

String MODEMBGXX::state_machine(uint32_t wait) {
	bool ip_status = false;

	loop(wait);

	return state;
}

// add radio and cops
void MODEMBGXX::join(uint8_t contextID) {

	if(contextID < 1 || contextID > MAX_CONNECTIONS)
		return;

	if(!has_context(contextID))
		open_pdp_context(contextID);

	return;

}

bool MODEMBGXX::config() {

	if(!st.gsm_ready){

		if (!check_command("ATE0", "OK", "ERROR")) return false;
		#ifdef DEBUG_BG95
		log("[config] echo mode off");
		#endif

		check_command("AT+CREG=2","OK","ERROR",3000); //

		//check_command("AT+QCFG=\"band\"","OK","ERROR",3000); //

		get_imei();
		#ifdef DEBUG_BG95
		log("[config] imei: " + imei);
		#endif
		delay(5000);

		check_command("AT+CSCS=\"IRA\"", "OK");

		if (check_command("AT+CMGF=1", "OK", "ERROR")){
			#ifdef DEBUG_BG95
			log("[config] sms text mode on");
			#endif
			st.gsm_ready = true;
		}else log("couldn't configure sms");
	}

	did_config = true;

	if(get_ccid() == ""){
		log("no sim card detected");
		return false;
	}

	get_imsi();

	return (imei != "");
}

bool MODEMBGXX::configure_radio_mode(uint8_t radio,uint8_t cops){

	if(radio == GPRS && technology != GPRS){
		if(cops != 0){
			if(!check_command("AT+COPS=4,2,\""+String(cops)+"\",0","OK","ERROR",45000))
				return false;
		}else{
			//check_command("AT+QCFG=\"iotopmode\",0,1","OK",3000);
			check_command("AT+QCFG=\"nwscanmode\",1,1","OK",3000);
		}
	}else if(radio == NB && technology != NB){
		if(cops != 0){
			if(!check_command("AT+COPS=4,2,\""+String(cops)+"\",9","OK","ERROR",45000))
			return false;
		}else{
			check_command("AT+QCFG=\"iotopmode\",1,1","OK",3000);
			check_command("AT+QCFG=\"nwscanmode\",0,1","OK",3000);
		}
	}else if(radio == CATM1 && technology != CATM1){
		if(cops!=0){
			//if(!check_command("AT+COPS=1,2,"+String(cops)+",8","OK","ERROR",45000)){
			if(!check_command("AT+COPS=4,2,\""+String(cops)+"\",8","OK","ERROR",45000))
				return false;
		}else{
			check_command("AT+QCFG=\"iotopmode\",0,1","OK",3000);
			check_command("AT+QCFG=\"nwscanmode\",0,1","OK",3000);
		}
	}else if(radio == AUTO){
		if(cops != 0){
			if(!check_command("AT+COPS=4,2,\""+String(cops)+"\"","OK","ERROR",45000))
				return false;
		}else{
			check_command("AT+QCFG=\"iotopmode\",2,1","OK",3000);
			check_command("AT+QCFG=\"nwscanmode\",0,1","OK",3000);
		}
	}
	else return false;

}

void MODEMBGXX::check_sms() {
	send_command("AT+CMGL=\"ALL\"");
	//send_command("AT+CMGL=\"REC UNREAD\"");
	delay(AT_WAIT_RESPONSE);

	String   sms[7]; // index, status, origin, phonebook, date, time, msg
	//uint8_t  index     = 0;
	uint32_t timeout   = millis() + 10000;
	bool     found_sms = false;
	uint8_t counter = 0;

	while (timeout >= millis()) {
		if (modem->available()) {
			String ret = modem->readStringUntil(AT_TERMINATOR);

			ret.trim();
			if (ret.length() == 0) continue;
			if (ret == "OK") break;
			if (ret == "ERROR") break;

			log("[sms] '" + ret + "'");

			String index = "", msg_state = "", origin = "", msg = "";

			if (!ret.startsWith("+CMGL:")) {
				parse_command_line(ret, true);
			}

			/*
			if (found_sms) continue;

			found_sms = true;
			*/
			ret = ret.substring(6);
			ret.trim();

			uint8_t word = 0, last_i = 0;
			for (uint8_t i = 0; i < ret.length(); i++) {
				if (ret.charAt(i) == ',') {
					switch (word){
						case 0:
							message[counter].used   = true;
							index = ret.substring(0,i);
							#ifdef DEBUG_BG95
							log("index: "+index);
							#endif
							message[counter].index  = (uint8_t) index.toInt();
							last_i = i;
							break;
						case 1:
							msg_state = ret.substring(last_i,i);
							#ifdef DEBUG_BG95
							log("msg_state: "+msg_state);
							#endif
							last_i = i;
							break;
						case 2:
							origin = ret.substring(last_i,i);
							origin.replace("\"","");
							origin.replace(",","");
							//origin.replace("+","");
							#ifdef DEBUG_BG95
							log("origin: "+origin);
							#endif
							memcpy(message[counter].origin,origin.c_str(),i-last_i);
							last_i = i;
							break;
						default:
							break;
					}
					word++;
				}
			}

			if (modem->available()) {
				String ret = modem->readStringUntil(AT_TERMINATOR);
				ret.trim();
				msg = ret;
				#ifdef DEBUG_BG95
				log("msg: "+msg);
				#endif
				memcpy(message[counter].msg,msg.c_str(),ret.length());
			}

			if(counter < MAX_SMS-1)
				counter++;
			else
				continue;

		} else {
			delay(AT_WAIT_RESPONSE);
		}
	}

	for(uint8_t i = 0; i<counter; i++){
		process_sms(i);
		message[i].used = false;
		memset(message[i].origin,0,20);
		memset(message[i].msg,0,256);
	}
	return;
}

void MODEMBGXX::process_sms(uint8_t index) {
	if (message[index].used && message[index].index >= 0) {
		message[index].used = false;

		sms_handler_func(message[index].index, String(message[index].origin), String(message[index].msg));
	}

	for (uint8_t index = 0; index < MAX_CONNECTIONS; index++) {
		if (!connected_state[index]) continue;

		connected_until[index] = millis() + CONNECTION_STATE;
	}
}

bool MODEMBGXX::sms_send(String origin, String message) {
	#ifdef DEBUG_BG95
	log("[sms] sending..");
	#endif
	// clean buffer
	while(modem->available())
		modem->read();

	if (!check_command_no_ok("AT+CMGS=\"" + origin + "\"", ">", "ERROR")) return false;
	if (!check_command(message + "\x1A", "OK", "ERROR")) return false;

	#ifdef DEBUG_BG95
	log("[sms] sent");
	#endif
	return true;
}

bool MODEMBGXX::sms_remove(uint8_t index) {
	#ifdef DEBUG_BG95
	log("[sms] removing " + String(index) + "..");
	#endif
	if (!check_command("AT+CMGD=" + String(index), "OK", "ERROR", 7000)) return false;
	#ifdef DEBUG_BG95
	log("[sms] removed");
	#endif
	return true;
}

bool MODEMBGXX::sms_handler() {
	return (sms_handler_func != NULL);
}

bool MODEMBGXX::sms_handler(void(*handler)(uint8_t, String, String)) {
	if (sms_handler()) return false;

	sms_handler_func = handler;

	return true;
}

bool MODEMBGXX::connect(uint8_t clientID, String proto, String host, uint16_t port, uint16_t wait) {
	if(apn_connected() != 1)
		return false;

	if (clientID >= MAX_TCP_CONNECTIONS) return false;

	close(clientID);

	uint8_t context = 1;
	if(check_command_no_ok("AT+QIOPEN="+String(context)+","+String(clientID) + ",\"" + proto + "\",\"" + host + "\","
	+ String(port),"+QIOPEN: "+String(clientID)+",0","ERROR"),10000){
		connected_state[clientID] = true;
		return true;
	}

	get_command("AT+QIGETERROR");

	return false;
}

bool MODEMBGXX::connect(uint8_t cid, uint8_t clientID, String proto, String host, uint16_t port, uint16_t wait) {
	if(apn_connected() != 1)
		return false;

	if(cid == 0 || cid > MAX_CONNECTIONS)
		return false;

	if (clientID >= MAX_TCP_CONNECTIONS) return false;

	close(clientID);

	if(check_command_no_ok("AT+QIOPEN="+String(cid)+","+String(clientID) + ",\"" + proto + "\",\"" + host + "\","
	+ String(port),"+QIOPEN: "+String(clientID)+",0","ERROR"),10000){
		connected_state[clientID] = true;
		return true;
	}

	get_command("AT+QIGETERROR");

	return false;
}

String MODEMBGXX::get_subscriber_number(uint16_t wait){
	return "";

	uint32_t timeout = millis() + wait;

	send_command("AT+CNUM");
	delay(AT_WAIT_RESPONSE);

	String number;

	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue;

			if (response != "ERROR" && isNumeric(response)) {
				#ifdef DEBUG_BG95
					log("[number] response = '" + response + "'");
				#endif
			}
		}

		delay(AT_WAIT_RESPONSE);
	}

	return "";

}

bool MODEMBGXX::connected(uint8_t index) {

	return connected_state[index];

}

void MODEMBGXX::check_connection_state(int8_t cid){
	if(cid < 0 || cid >= MAX_CONNECTIONS)
		return;

	String query = "AT+QISTATE=1,"+String(cid);
	String response = get_command(query);

	int16_t index = 0;
	int8_t count = 0;
	int8_t connect_id = -1;
	String param = "";

	while(true){
		index = response.indexOf(",");
		if(index == -1)
			break;
		count++;
		param = response.substring(0,index);
		if(count == 6){
			int state = param.toInt();
			if(state == 2)
				connected_state[cid] = true;
			else connected_state[cid] = false;
		}
		response = response.substring(index+1);
	}

}

bool MODEMBGXX::close(uint8_t index) {
	connected_state[index] = false;
	connected_since[index] = 0;
	data_pending[index] = false;

	return check_command("AT+QICLOSE=" + String(index),"OK", "ERROR", 10000);
}

bool MODEMBGXX::send(uint8_t connect_id, uint8_t *data, uint16_t size) {
	if (connect_id >= MAX_CONNECTIONS) return false;
	if (connected(connect_id) == 0) return false;

	while (modem->available()) {
		String line = modem->readStringUntil(AT_TERMINATOR);

		line.trim();

		parse_command_line(line, true);
	}

	while (modem->available()) modem->read(); // delete garbage on buffer

	if (!check_command_no_ok("AT+QISEND=" + String(connect_id) + "," + String(size), ">", "ERROR")) return false;

	send_command(data, size);
	delay(AT_WAIT_RESPONSE);

	uint32_t timeout        = millis() + 10000;
	String new_data_command = "";

	while (timeout >= millis()) {
		if (modem->available()) {
			String line = modem->readStringUntil(AT_TERMINATOR);

			line.trim();

			if (line.length() == 0) continue;

			//log("[send] ? '" + line + "'");
			#ifdef DEBUG_BG95
			log("parse: "+line);
			#endif
			parse_command_line(line, true);

			if (line.indexOf("SEND OK") > -1 || line.indexOf("OK") > -1) return true;

		}

		delay(AT_WAIT_RESPONSE);
	}

	check_data_pending();

	return false;
}

void MODEMBGXX::check_modem_buffers() {
	for (uint8_t cid = 0; cid < MAX_CONNECTIONS; cid++) {
		if(connected_state[cid] && cid != 2) // 2 is reserverd for MQTT
			data_pending[cid] = true;
	}
}

String MODEMBGXX::check_messages() {

	String command = "";
	while (modem->available() > 0) {

		command = modem->readStringUntil(AT_TERMINATOR);

		command.trim();

		#ifdef DEBUG_BG95_HIGH
		log("[command] '" + command + "'");
		#endif

		if (command.length() == 0) continue;
		// if (command.length() == 1 && command.startsWith(">")) continue;
		// if (command.length() == 2 && command.startsWith("> ")) continue;

		command = parse_command_line(command, true);
	}
	return command;
}

String MODEMBGXX::parse_command_line(String line, bool set_data_pending) {
	//log("parse: "+line);

	String _cgreg = "+CGREG: ";
	String _cereg = "+CEREG: ";
	String _creg = "+CREG: ";
	int8_t index = -1;

	if(line.startsWith("AT+")){
		log("echo is enabled, disable it");
		send_command("ATE0");
	}

	if (line.startsWith(_cgreg)) {
		index = line.indexOf(",");
		if(index > -1)
			line = line.substring(index+1,index+2);
		else line = line.substring(_cgreg.length(),_cgreg.length()+1);
		if(isNumeric(line)){
			radio_state = line.toInt();
			#ifdef DEBUG_BG95
			switch(radio_state){
				case 0:
					log("EGPRS not registered");
					st.apn_gsm = false;
					break;
				case 1:
					//log("EGPRS registered");
					st.apn_gsm = true;
					break;
				case 2:
					log("EGPRS connecting");
					st.apn_gsm = false;
					break;
				case 3:
					log("EGPRS registration denied");
					st.apn_gsm = false;
					break;
				case 4:
					//log("EGPRS Unknow");
					st.apn_gsm = false;
					break;
				case 5:
					log("EGPRS registered, roaming");
					st.apn_gsm = false;
					break;
			}
			#endif
		}
		return "";
	}else if (line.startsWith(_cereg)) {
		index = line.indexOf(",");
		if(index > -1)
			line = line.substring(index+1,index+2);
		else line = line.substring(_cereg.length(),_cereg.length()+1);
		if(isNumeric(line)){
			radio_state = line.toInt();
			switch(radio_state){
				case 0:
					log("LTE not registered");
					st.apn_lte = false;
					break;
				case 1:
					//log("LTE registered");
					st.apn_lte = true;
					break;
				case 2:
					log("LTE connecting");
					st.apn_lte = false;
					break;
				case 3:
					log("LTE registration denied");
					st.apn_lte = false;
					break;
				case 4:
					#ifdef DEBUG_BG95_HIGH
					//log("LTE Unknow");
					#endif
					st.apn_lte = false;
					break;
				case 5:
					log("LTE registered, roaming");
					st.apn_lte = true;
					break;
			}
		}
		return "";
	}else if (line.startsWith(_creg)) {
		String connected_ = "";
		String technology_ = "";

		index = line.indexOf(",");
		if(index > -1){
			line = line.substring(index+1);
			index = line.indexOf(",");
			if(index > -1){
				connected_ = line.substring(0,index);
				//log("connected: "+String(connected_));
				line = line.substring(index+1);
			}else{
				connected_ = line;
				//log("connected: "+String(connected_));
			}
		}else return "";

		index = line.indexOf(",");
		if(index > -1){
			line = line.substring(index+1);
			index = line.indexOf(",");
			if(index > -1){
				technology_ = line.substring(index+1);
				//log("technology: "+String(technology_));
			}else return "";
		}else return "";

		if(isNumeric(connected_)){
			int8_t act = -1;
			radio_state = connected_.toInt();
			if(technology_ != "")
				act = technology_.toInt();
			else act = -1;

			switch(radio_state){
				case 0:
					 st.apn_lte = false;
					 st.apn_gsm = false;
					break;
				case 1:
					if(act == 0){
						st.apn_gsm = true;
						st.apn_lte = false;
					}else if(act == 9){
						st.apn_gsm = false;
						st.apn_lte = true;
					}
					break;
				case 2:
					if(act == 0){
						//st.apn_gsm = 2;
						st.apn_gsm = false;
						st.apn_lte = false;
					}else if(act == 9){
						st.apn_gsm = false;
						//st.apn_lte = 2;
						st.apn_lte = false;
					}
					break;
				case 3:
					st.apn_gsm = false;
					st.apn_lte = false;
					break;
				case 4:
					st.apn_gsm = false;
					st.apn_lte = false;
					break;
				case 5:
					if(act == 0){
						st.apn_gsm = true;
						st.apn_lte = false;
					}else if(act == 9){
						st.apn_gsm = false;
						st.apn_lte = true;
					}
					break;
			}
		}

	}else if (line.startsWith("+QIOPEN:")) {

		int8_t index = line.indexOf(",");
		int8_t cid = 0, state = -1;
		if(index > -1){
			cid = line.substring(index-1,index).toInt();
			state = line.substring(index+1).toInt();
		}
		if(state == 0){
			#ifdef DEBUG_BG95
			log("TCP is connected");
			#endif
			connected_state[cid] = true;
		}else{
			log("Error opening TCP");
			connected_state[cid] = false;
		}
		/*
		for (uint8_t index = 0; index < MAX_CONNECTIONS; index++) {
			if (!line.startsWith("C: " + String(index) + ",")) continue;

			connected_until[index] = millis() + CONNECTION_STATE;
			connected_state[index] = line.endsWith("\"CONNECTED\"");

			#ifdef DEBUG_BG95
			if (connected_state[index]) log("socket " + String(index) + " = connected");
			#endif
		}
		*/
		return "";
	}else if (line.startsWith("+QIURC: \"recv\",") ) {
		String cid_str = line.substring(15);
		log("cid_str: "+cid_str);
		uint8_t cid = cid_str.toInt();
		if (set_data_pending) {
			data_pending[cid] = true;
			return "";
		} else {
			read_buffer(cid);
			return "";
		}
	}else if (line.startsWith("+QIURC: \"closed\",") ) {
		#ifdef DEBUG_BG95
		log("QIURC closed: "+line);
		#endif
		int8_t index = line.indexOf(",");
		int8_t cid = -1;
		if(index > -1){
			state = state.substring(index+1,index+1);
			cid = state.toInt();
			log("connection: "+String(cid)+" closed");
			connected_state[cid] = false;
		}
	}else if (line.startsWith("+CMTI")){
		check_sms();
		return "";
	}else if (line.startsWith("+QIACT: ")) {
		line = line.substring(8);
		int8_t index = line.indexOf(",");
		uint8_t cid = 0;
		if(index > -1){
			cid = line.substring(0,index).toInt(); // connection
			line = line.substring(index+1);
		}else{
			return "";
		}

		if(cid == 0 || cid > MAX_CONNECTIONS)
			return "";

		int8_t state = line.substring(0,1).toInt(); // connection
		if(state == 1){
			#ifdef DEBUG_BG95_HIGH
			log("network connected: "+String(cid));
			#endif
			connected_state[cid-1] = true;
		}else{
			#ifdef DEBUG_BG95
			log("network disconnected: "+String(cid));
			#endif
			connected_state[cid-1] = false;
		}

	}else if(line.startsWith("+QMTSTAT")){
		// error ocurred, channel is disconnected
		String filter = "+QMTSTAT: ";
		uint8_t index = line.indexOf(filter);
		line = line.substring(index);
		index = line.indexOf(",");
		if(index > -1){
			String client = line.substring(filter.length(),index);
			#ifdef DEBUG_BG95_HIGH
			log("client: "+client);
			#endif
			if(isNumeric(client)){
				uint8_t id = client.toInt();
				if(id <= MAX_MQTT_CONNECTIONS)
					mqtt[id].socket_state = false;
				#ifdef DEBUG_BG95
				log("MQTT closed");
				#endif
			}
		}
	}
	else if(line.startsWith("+QMTRECV:")){
		return mqtt_message_received(line);
	}else if (line.startsWith("+QMTCONN: ")) {
		String filter = "+QMTCONN: ";
		line = line.substring(filter.length());
		index = line.indexOf(",");
		if(index > -1){
			uint8_t cidx = line.substring(0,index).toInt();
			if(cidx < MAX_MQTT_CONNECTIONS){
				String state = line.substring(index+1,index+2);
				if(isdigit(state.c_str()[0])){
					mqtt[cidx].socket_state = (int)state.toInt();
					mqtt_connected_state[cidx] = (int)(state.toInt()==MQTT_STATE_CONNECTED);
					#ifdef DEBUG_BG95
					if(mqtt_connected_state[cidx])
						log("mqtt client "+String(cidx)+" is connected");
					else
						log("mqtt client "+String(cidx)+" is disconnected");
					#endif
					return "";
				}

			}
		}
	}else if(line.startsWith("OK"))
		return "";
	//connected_state[index]
	return line;
}

String MODEMBGXX::mqtt_message_received(String line){

	String filter = "+QMTRECV: ";
	int index = line.indexOf(",");
	if(index > -1){ // filter found
		String aux = line.substring(filter.length(),index); // client id
		if(isNumeric(aux)){
			uint8_t client_id = aux.toInt();
		}else return "";

		aux = line.substring(index+1,index+2); // channel
		if(isNumeric(aux)){
			uint8_t channel = (uint8_t)aux.toInt();
			if(channel < 5)
				mqtt_buffer[channel] = 0; // I do not know the length of the payload that will be read
			line = line.substring(index+2); // null
		}else return "";

		index = line.indexOf(",");
		if(index > -1){ // has payload
			line = line.substring(index+1);
			index = line.indexOf(",");
			if(index > -1){
				String topic = line.substring(0,index);
				String payload = line.substring(index+1);
				if(parseMQTTmessage != NULL)
					parseMQTTmessage(topic,payload);
			}
		}
	}

	return "";
}

void MODEMBGXX::check_data_pending() {
	for (uint8_t index = 0; index < MAX_CONNECTIONS; index++) {
		if (!data_pending[index]) continue;

		//read_buffer(index);
	}
}

bool MODEMBGXX::has_data_pending(uint8_t index){
	if(index >= 0 && index < MAX_CONNECTIONS)
		return data_pending[index];
	return false;
}

uint16_t MODEMBGXX::recv(uint8_t index, uint8_t *data, uint16_t size) {
	if (buffer_len[index] == 0) return 0;

	uint16_t i;

	if (buffer_len[index] < size) {
		size = buffer_len[index];

		for (i = 0; i < size; i++) {
			data[i] = buffers[index][i];
		}

		buffer_len[index] = 0;

		return size;
	}

	for (i = 0; i < size; i++) {
		data[i] = buffers[index][i];
	}

	for (i = size; i < buffer_len[index]; i++) {
		buffers[index][i - size] = buffers[index][i];
	}

	buffer_len[index] -= size;

	return size;
}

void MODEMBGXX::read_buffer(uint8_t index, uint16_t wait) {

	while (modem->available())
		modem->readStringUntil(AT_TERMINATOR);

	int16_t left_space = CONNECTION_BUFFER-buffer_len[index];
	if( left_space <= 10)
		return;

	send_command("AT+QIRD=" + String(index) + "," + String(left_space));

	delay(AT_WAIT_RESPONSE);

	uint32_t timeout = millis() + wait;
	String info = "";
	bool end = false;

	while (timeout >= millis() && !end) {
		while (modem->available()) {
			info = modem->readStringUntil(AT_TERMINATOR);

			info.trim();

			if (info.length() == 0) continue;

			/*
			if (info.startsWith("+CIPRXGET: 1," + String(index))) {
				read_data(index, info);
				break;
			}
			*/
			if (info.startsWith("+QIRD: ")) {

				log(info); // +QIRD
				info = info.substring(7);
				String size = info.substring(0,1);
				uint16_t len = info.toInt();
				if(len > 0){
					if(len + buffer_len[index] <= CONNECTION_BUFFER){
						uint16_t n = modem->readBytes(&buffers[index][buffer_len[index]], len);
						buffer_len[index] += n;
					}else{
						log("buffer is full, data read after this will be discarded");
					}
				}
				if(buffer_len[index] == 0)
					data_pending[index] = false;
				else
					data_pending[index] = true;

				break;
			}else if (info == "OK") {
				end = true;
				return;
			}else if(info == "ERROR"){
				if(buffer_len[index] == 0)
					data_pending[index] = false;
				else
					data_pending[index] = true;
				return;
			}else{
				parse_command_line(info);
			}
		}

		delay(AT_WAIT_RESPONSE);
	}

}

// deprecated
void MODEMBGXX::read_data(uint8_t index, String command, uint16_t bytes) {
	// 4 assumes MAX_CONNECTIONS will be under 10 (0 to 9)
	#ifdef DEBUG_BG95_HIGH
	log("[read_data] '" + command + "'");
	#endif

	//command = command.substring(4, command.length() - 2);

	//bytes = command.toInt();

	#ifdef DEBUG_BG95_HIGH
	log("[read_data] available bytes (" + String(index) + ") = " + String(bytes));
	#endif

	if (bytes == 0) return;

	if(bytes + index < CONNECTION_BUFFER){
		command.toCharArray(&buffers[index][buffer_len[index]],bytes);
		buffer_len[index] += bytes;
	}else log("buffer is full");

	/**
	 * This previous read can take "forever" and expire the connection state,
	 * so in this particular case, we will assume the connection is still valid
	 * and reset the timestamp.
	 **/
	connected_until[index] = millis() + CONNECTION_STATE;

	#ifdef DEBUG_BG95_HIGH
	log("[read_data] all bytes read, in buffer " + String(buffer_len[index]) + " bytes");
	#endif

	while (modem->available()) {
		String line = modem->readStringUntil(AT_TERMINATOR);

		line.trim();

		if (line == "OK") break;

		if (!parse_command_line(line, true)) {
			#ifdef DEBUG_BG95_HIGH
			log("[read_data] ? = '" + line + "'");
			#endif
		}
	}

	return check_data_pending();
}

bool MODEMBGXX::open_pdp_context(uint8_t tcp_cid) {
	if(tcp_cid < 1 || tcp_cid > MAX_CONNECTIONS)
		return false;

	return check_command("AT+QIACT="+String(tcp_cid), "OK", "ERROR",30000);
}

bool MODEMBGXX::close_pdp_context(uint8_t tcp_cid) {

	if(tcp_cid < 1 || tcp_cid > MAX_CONNECTIONS)
		return false;

	return check_command("AT+QIDEACT="+String(tcp_cid), "OK", "ERROR");
}

bool MODEMBGXX::disable_pdp(uint8_t tcp_cid) {

	if(tcp_cid < 1 || tcp_cid > MAX_CONNECTIONS)
		return false;

	if (!check_command("AT+CGACT=0,"+String(tcp_cid), "OK", "ERROR", 10000)) return false;

	#ifdef DEBUG_BG95
	log("PDP disabled");
	#endif

	return true;
}

bool MODEMBGXX::enable_pdp(uint8_t tcp_cid) {

	if(tcp_cid < 1 || tcp_cid > MAX_CONNECTIONS)
		return false;

	if (!check_command("AT+CGACT=1,"+String(tcp_cid), "OK", "ERROR", 10000)) return false;

	#ifdef DEBUG_BG95
	log("PDP enabled");
	#endif

	return true;
}

int16_t MODEMBGXX::get_rssi() {

	if (rssi_until > millis()) {
		return rssi_last;
	}

	rssi_until = millis() + 20000;

	String command = "AT+QCSQ";
	String response = get_command(command,"+QCSQ: ",300);

	response.trim();
	if (response.length() == 0) return 0;

	int p = response.indexOf(",");
	tech = response.substring(0,p);
	tech.replace("\"","");
	#ifdef DEBUG_BG95_HIGH
	log("tech in use: "+tech);
	#endif
	if(tech == "NBIoT")
		technology = NB;
	else if(tech == "GSM")
		technology = GPRS;
	else if(tech == "eMTC")
		technology = CATM1;
	else if(tech == "NOSERVICE")
		technology = -1;

	response = response.substring(p+1);
	p = response.indexOf(",");
	if(p > -1)
		response = response.substring(0, p);

	int rssi = 0;
	if(isNumeric(response)){
		rssi = response.toInt();
		#ifdef DEBUG_BG95_HIGH
		log("rssi: "+response);
		#endif
	}

	if(rssi != 0)
		rssi_last = rssi;

	rssi_until = millis() + 5000;
	return rssi_last;

	/*
	for CSQ command
	if (rssi == 0) {
		rssi_last = -115;
	} else if (rssi == 1) {
		rssi_last = -111;
	} else if (rssi == 31) {
		rssi_last = -52;
	} else if (rssi >= 2 && rssi <= 30) {
		rssi_last = -110 + (rssi * 56 / 29);
	} else {
		rssi_last = 99;
	}
	*/
}

int16_t MODEMBGXX::rssi(){

	return rssi_last;
}

// use it to get network clock
bool MODEMBGXX::get_clock(tm* t){
	String response = get_command("AT+CCLK?","+CCLK: ",300);

	if(response.length() == 0)
		return false;

	uint8_t index = 0;
	#ifdef DEBUG_BG95_HIGH
	log("response: "+response);
	#endif
	index = response.indexOf("/");
	if(index == -1)
		return false;

	t->tm_year = (int)response.substring(index-2,index).toInt();
	response = response.substring(index+1);

	//log("year: "+String(t->tm_year));
	//log("response: "+response);

	index = response.indexOf("/");
	if(index == -1)
		return false;

	t->tm_mon = (int)response.substring(index-2,index).toInt();
	response = response.substring(index+1);

	//log("month: "+String(t->tm_mon));
	//log("response: "+response);

	index = response.indexOf(",");
	if(index > 0)
		t->tm_mday = (int)response.substring(index-2,index).toInt();
	response = response.substring(index+1);

	//log("day: "+String(t->tm_mday));
	//log("response: "+response);

	index = response.indexOf(":");
	if(index == -1)
		return false;

	t->tm_hour = (int)response.substring(index-2,index).toInt();
	response = response.substring(index+1);

	//log("hour: "+String(t->tm_hour));
	//log("response: "+response);

	index = response.indexOf(":");
	if(index == -1)
		return false;

	t->tm_min = (int)response.substring(index-2,index).toInt();
	response = response.substring(index+1);

	//log("min: "+String(t->tm_min));
	//log("response: "+response);

	bool minus = false;
	index = response.indexOf("+");
	if(index == -1){
		index = response.indexOf("-");
		minus = true;
	}

	t->tm_sec = (int)response.substring(index-2,index).toInt();
	response = response.substring(index+1);

	//log("sec: "+String(t->tm_sec));

	if(t->tm_year < 19)
		return false;

	if(response.toInt() != 0){
		if(minus){
			tz = -response.toInt()/4*3600;
			tz -= response.toInt()%4*15*60;

			t->tm_hour -= response.toInt()/4;
			t->tm_min -= response.toInt()%4*15;
		}else{
			tz = response.toInt()/4*3600;
			tz += response.toInt()%4*15*60;

			t->tm_hour += response.toInt()/4;
			t->tm_min += response.toInt()%4*15;
		}
	}
	#ifdef DEBUG_BG95
	log("tz: "+String(tz));
	#endif

  int y = t->tm_year;
  int mo = t->tm_mon;
  int d = t->tm_mday;
  int h = t->tm_hour;
  int m = t->tm_min;
  int s = t->tm_sec;

	Serial.printf(" %d %d %d %d:%d:%d \n",y,mo,d,h,m,s);
	/*
  setTime(h, m, s, d, mo, y);

	#ifdef DEBUG_BG95
		log("[clock] response = '" + String(time) + "'");
	#endif
	*/
	return true;
}

int32_t MODEMBGXX::get_tz(){
	return tz;
}

void MODEMBGXX::sync_clock_ntp(bool force){

	if(clock_sync_timeout<millis() || force)
		clock_sync_timeout = millis()+(3600*1000);
	else return;

	//get_command_no_ok("AT+QNTP=1,\"202.120.2.101\",123","+QNTP: ",60000);
	get_command("AT+QNTP=1,\"pool.ntp.org\",123","+QNTP: ",60000);
}

// not needed
String MODEMBGXX::scan_cells(){
	#ifdef DEBUG_BG95
	log("getting cells information..");
	#endif

	//send_command("AT+CNETSCAN");
	send_command("AT+QENG=\"NEIGHBOURCELL\"");
	delay(AT_WAIT_RESPONSE);

	uint32_t timeout = millis() + 50000;
	String cells = "";

	uint16_t counter = 0;
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);
			counter += response.length();
			response.trim();

			if (response.length() == 0) continue;

			log(response);

			if (response == "ERROR")
				return "";
			else if(response == "OK"){
				#ifdef DEBUG_BG95
				log("[cells info] response = '" + response + "'");
				#endif
				return cells;
			}else cells += response;

			if(counter >= 255){
				log("!! overflow");
				return cells;
			}
		}

		delay(AT_WAIT_RESPONSE);
	}
	return cells;
}

String MODEMBGXX::get_position(){
	#ifdef DEBUG_BG95
	log("getting cells information..");
	#endif
	log(get_command("AT+QGPSCFG=\"gnssconfig\""));

	if (!check_command("AT+QGPS=1", "OK", "ERROR", 400)) return "";

	String response = "";
	//uint32_t timeout = millis() + 120000;
	uint32_t timeout = millis() + 20000;
	while(millis() < timeout){
		response = get_command("AT+QGPSLOC=2","+QGPSLOC: ",300);
		#ifdef DEBUG_BG95
		log(response);
		log("response len: "+String(response.length()));
		#endif
		if(response.length() > 0){
			check_command("AT+QGPSEND", "OK", "ERROR", 400);
			return response;
		}
		delay(2000);
	}

	check_command("AT+QGPSEND", "OK", "ERROR", 400);
	/*

	check_command("AT+QGPS=1", "OK", "ERROR", 400);

	check_command("AT+QGPSCFG=\"nmeasrc\",1", "OK", "ERROR", 400);

	check_command("AT+QGPSGNMEA=\"GGA\"", "OK", "ERROR", 400);

	check_command("AT+QGPSGNMEA=\"RMC\"", "OK", "ERROR", 400);

	check_command("AT+QGPSGNMEA=\"GSV\"", "OK", "ERROR", 400);

	check_command("AT+QGPSGNMEA=\"GSA\"", "OK", "ERROR", 400);

	check_command("AT+QGPSGNMEA=\"VTG\"", "OK", "ERROR", 400);

	check_command("AT+QGPSGNMEA=\"GNS\"", "OK", "ERROR", 400);

	check_command("AT+QGPSCFG=\"nmeasrc\",0", "OK", "ERROR", 400);
	*/
	return response;
}

bool MODEMBGXX::switch_radio_off(){
	#ifdef DEBUG_BG95
	log("switch radio off");
	#endif
	if(check_command("AT+CFUN=0","OK","ERROR",15000)){
		st.gsm_ready = false;
		st.apn_ready = false;
		st.apn_lte = false;
		st.apn_gsm = false;
		return true;
	}else return false;
}

String MODEMBGXX::get_imei(uint32_t wait) {
	if(imei != "")
		return imei;

	String command = "AT+CGSN";
	imei = get_command(command,300);
	return imei;
}

String MODEMBGXX::get_ccid(uint32_t wait) {
	uint32_t timeout = millis() + wait;

	String command = "AT+QCCID";
	return get_command(command,"+QCCID: ",1000);
}

String MODEMBGXX::get_imsi(uint32_t wait) {
	uint32_t timeout = millis() + wait;

	String command = "AT+CIMI";
	return get_command(command,300);
}

String MODEMBGXX::get_ip(uint8_t cid, uint32_t wait) {
	if(cid == 0 || cid > MAX_CONNECTIONS)
		return "";

	uint32_t timeout = millis() + wait;

	String command = "AT+CGPADDR="+String(cid);
	return get_command(command,"+CGPADDR: "+String(cid)+",",300);
}

// --- MQTT ---
/*
* init mqtt
*
* @callback - register callback to parse mqtt messages
*/
void MODEMBGXX::MQTT_init(bool(*callback)(String,String)) {
	parseMQTTmessage = callback;

	uint8_t i = 0;
	while(i<MAX_MQTT_CONNECTIONS){
		mqtt[i++].active = false;
	}
}

/*
* setup mqtt
*
* @clientID - supports 5 clients, yet is limited to MAX_MQTT_CONNECTIONS
* @contextID - index of TCP tcp[] - choose 1 connection
* @will_topic - topic to be sent if mqtt loses connection
* @payload - payload to be sent with will topic
*
* returns true if configuration was succeed
*/
bool MODEMBGXX::MQTT_setup(uint8_t clientID, uint8_t contextID, String will_topic, String payload) {

	if(clientID >= MAX_MQTT_CONNECTIONS)
		return false;

	mqtt[clientID].contextID = contextID;
	mqtt[clientID].clientID = clientID;
	mqtt[clientID].active = true;


	String s = "AT+QMTCFG=\"pdpcid\","+String(clientID)+","+String(mqtt[clientID].contextID);
	check_command(s.c_str(),"OK",2000);
		//return false;

	s = "AT+QMTCFG=\"recv/mode\","+String(clientID)+",0";
	check_command(s.c_str(),"OK",2000);
		//return false;

	s = "AT+QMTCFG=\"will\","+String(clientID)+",1,2,1,\""+will_topic+"\",\""+payload+"\"";
	check_command(s.c_str(),"OK",2000);
		//return false;

	mqtt[clientID].configured = true;

	return true;
}

/*
* Connects to a mqtt broker
*
* @clientID: 0-5, limited to MAX_CONNECTIONS
* @uid: id to register device on broker
* @uid: uid to register device on broker
* @user: credential
* @pass: credential
* @host: DNS or IP
* @port: default 1883
*
* return true if connection is open
*/
bool MODEMBGXX::MQTT_connect(uint8_t clientID, const char* uid, const char* user, const char* pass, const char* host, uint16_t port) {
	if(clientID >= MAX_CONNECTIONS)
		return false;

	#ifdef DEBUG_BG95
	log("connecting to: "+String(host));
	#endif
	if(!MQTT_isOpened(clientID,host,port)){
		if(!MQTT_open(clientID,host,port))
			return false;
	}

	uint8_t state = mqtt[clientID].socket_state;

	if(state == MQTT_STATE_DISCONNECTING){
		MQTT_close(clientID);
		MQTT_disconnect(clientID);
		MQTT_readAllBuffers(clientID);
		return false;
	}

	if(state != MQTT_STATE_CONNECTING && state != MQTT_STATE_CONNECTED){
		String s = "AT+QMTCONN="+String(clientID)+",\""+String(uid)+"\",\""+String(user)+"\",\""+String(pass)+"\"";
		if(check_command_no_ok(s.c_str(),"+QMTCONN: "+String(clientID)+",0,0",5000))
			mqtt[clientID].socket_state = MQTT_STATE_CONNECTED;
	}

	if(mqtt[clientID].socket_state == MQTT_STATE_CONNECTED){
		mqtt_connected_state[clientID] = true;
	}

	return mqtt_connected_state[clientID];
}

/*
* return true if connection is open
*/
bool MODEMBGXX::MQTT_connected(uint8_t clientID){
	if(clientID > MAX_MQTT_CONNECTIONS)
		return false;

	return mqtt_connected_state[clientID];
}

/*
* 0 Failed to close connection
*	1 Connection closed successfully
*/
int8_t MODEMBGXX::MQTT_disconnect(uint8_t clientID) {
	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	String s = "AT+QMTDISC="+String(clientID);
	String f = "+QMTDISC: "+String(clientID)+",";
	String response = get_command(s.c_str(),f.c_str(),5000);

	if(response.length() > 0){
		int8_t rsp = -1;
		if(isdigit(response.c_str()[0]))
			rsp = (int)response.toInt();

		if(rsp == 0)
			mqtt_connected_state[clientID] = false;

		return (mqtt_connected_state[clientID] == 0);
	}else return 0;
}

/*
* return true if has succeed
*/
bool MODEMBGXX::MQTT_subscribeTopic(uint8_t clientID, uint16_t msg_id, String topic,uint8_t qos) {
	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	String s;
	s.reserve(512);
	s = "AT+QMTSUB="+String(clientID)+","+String(msg_id);
	s += ",\""+topic+"\","+String(qos);

	String f = "+QMTSUB: "+String(clientID)+","+String(msg_id)+",";
	//logging.println("BGxx","String: ",s);
	//String response = getAtCommandResponseNoOK(s.c_str(),f.c_str(),18000);
	String response = get_command_no_ok(s.c_str(),f.c_str(),18000);
	int8_t index = response.indexOf(",");
	#ifdef DEBUG_BG95_HIGH
	log("response: "+String(response));
	#endif
	if(index > -1){
		response = response.substring(0,index);
		if(isNumeric(response)){
			if((int8_t)response.toInt() == 0){
				#ifdef DEBUG_BG95_HIGH
				log("packet sent successfully");
				#endif
				return true;
			}
		}
	}
	return false;
}


/*
* return true if has succeed
*/
bool MODEMBGXX::MQTT_subscribeTopics(uint8_t clientID, uint16_t msg_id, String topic[],uint8_t qos[], uint8_t len) {
	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	String s;
	s.reserve(512);
	s = "AT+QMTSUB="+String(clientID)+","+String(msg_id);
	uint8_t i = 0;
	while(i<len){
		s += ",\""+topic[i]+"\","+String(qos[i]);
		i++;
	}

	String f = "+QMTSUB: "+String(clientID)+","+String(msg_id)+",";

	String response = get_command_no_ok(s.c_str(),f.c_str(),18000);
	int8_t index = response.indexOf(",");
	#ifdef DEBUG_BG95_HIGH
	log("response: "+String(response));
	#endif
	if(index > -1){
		response = response.substring(0,index);
		if(isNumeric(response)){
			if((int8_t)response.toInt() == 0){
				#ifdef DEBUG_BG95_HIGH
				log("packet sent successfully");
				#endif
				return true;
			}
		}
	}
	return false;
}

/*
* return true if has succeed
*/
int8_t MODEMBGXX::MQTT_unSubscribeTopic(uint8_t clientID, uint16_t msg_id, String topic[], uint8_t len) {
	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	String s = "AT+QMTUNS="+String(clientID)+","+String(msg_id);
	uint8_t i = 0;
	while(i<len){
		s += ","+topic[i];
		i++;
	}

	String f = "+QMTUNS: "+String(clientID)+","+String(msg_id)+",";
	String response = get_command(s.c_str(),f.c_str(),10000);
	response = response.substring(0,1);
	return (int8_t)response.toInt();
}

/*
*	return
*	-1 error
*	0 Packet sent successfully and ACK received from server (message that published when <qos>=0 does not require ACK)
*	1 Packet retransmission
*	2 Failed to send packet
*/
int8_t MODEMBGXX::MQTT_publish(uint8_t clientID, uint16_t msg_id,uint8_t qos, uint8_t retain, String topic, String msg) {
	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	if(!mqtt_connected_state[clientID]) return -1;

	String payload = msg;

	String s = "AT+QMTPUBEX="+String(clientID)+","+String(msg_id)+","+String(qos)+","+String(retain)+",\""+topic+"\",\""+payload+"\"";
	String f = "+QMTPUB: "+String(clientID)+","+String(msg_id)+",";

	String response = get_command_no_ok_critical(s.c_str(),f.c_str(),15000);
	response = response.substring(0,1);
	if(response.length() > 0){
		if(isdigit(response.c_str()[0])){
			#ifdef DEBUG_BG95_HIGH
			log("message sent");
			#endif
			return (int)response.toInt();
		}
	}
	return -1;
}

/*
* Forces reading data from mqtt modem buffers
* call it only if unsolicited messages are not being processed
*/
void MODEMBGXX::MQTT_readAllBuffers(uint8_t clientID) {
	if(clientID >= MAX_CONNECTIONS)
		return;

	String s = "";
	int8_t i = 0;

	if(MQTT_connected(clientID)){
		while(i<5){
			s = "AT+QMTRECV="+String(clientID)+","+String(i);
			get_command(s.c_str(),400);
			mqtt_buffer[i++] = -1;
		}
	}

	return;
}


/*
* private - Updates mqtt.socket_state of each initialized connection
*
*	0 MQTT is disconnected
*	1 MQTT is initializing
*	2 MQTT is connecting
*	3 MQTT is connected
*	4 MQTT is disconnecting
*/
void MODEMBGXX::MQTT_checkConnection(){

	String s = "AT+QMTCONN?";
	String res = get_command(s.c_str(),2000);

	return;
}

/*
* private
*/
bool MODEMBGXX::MQTT_open(uint8_t clientID, const char* host, uint16_t port) {

	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	if(MQTT_connected(clientID)){
		mqtt_connected_state[clientID] = true;
	}else{
		MQTT_close(clientID);
		String s = "AT+QMTOPEN="+String(clientID)+",\""+String(host)+"\","+String(port);
		if(check_command_no_ok(s.c_str(),"+QMTOPEN: "+String(clientID)+",0",5000))
			mqtt_connected_state[clientID] = true;
		else mqtt_connected_state[clientID] = false;
	}

	return mqtt_connected_state[clientID];
}
/*
* private
*/
bool MODEMBGXX::MQTT_isOpened(uint8_t clientID, const char* host, uint16_t port) {

	String s = "AT+QMTOPEN?";//+to_string(clientID)+",\""+string(settings.mqtt.host)+"\","+to_string(settings.mqtt.port);
	String ans = "+QMTOPEN: "+String(clientID)+",\""+String(host)+"\","+String(port);

	return check_command(s.c_str(),ans.c_str(),2000);
}
/*
* private
*/
bool MODEMBGXX::MQTT_close(uint8_t clientID) {
	if(clientID >= MAX_CONNECTIONS)
		return clientID;

	String s = "AT+QMTCLOSE="+String(clientID);
	String f = "+QMTCLOSE: "+String(clientID)+",";
	String response = get_command(s.c_str(),f.c_str(),10000);

	if(response.length() > 0){
		if(isdigit(response.c_str()[0])){
			mqtt_connected_state[clientID] = false;
			return (int)response.toInt() == 0;
		}
	}

	return false;
}
/*
* private - Reads data on mqtt modem buffers. Call it with high frequency
* No need to use if mqtt messages came on unsolicited response (current configuration)
*/
void MODEMBGXX::MQTT_readMessages(uint8_t clientID) {
	if(clientID >= MAX_CONNECTIONS)
		return;

	String s = "";
	int8_t i = 0;
	bool read = false;

	/* check if a buffer has messages */
	while(i<5){
		if(mqtt_buffer[i] != -1){
			s = "AT+QMTRECV="+String(clientID)+","+String(i);
			get_command(s.c_str(),400);
			mqtt_buffer[i] = -1;
		}
		i++;
	}

	return;
}

// --- --- ---

// --- private ---
void MODEMBGXX::send_command(String command, bool mute) {

	#ifdef DEBUG_BG95_HIGH
	if (!mute) log(">> " + command);
	#endif

	modem->println(command);
	modem->flush();

}

void MODEMBGXX::send_command(uint8_t *command, uint16_t size) {

	String data = "";

	if (modem->available()) {
		String response = modem->readStringUntil(AT_TERMINATOR);
		response.trim();

		if (response.length() != 0){
			#ifdef DEBUG_BG95_HIGH
			log("<< " +response);
			#endif

			data += parse_command_line(response);
		}
	}

	delay(AT_WAIT_RESPONSE);


	#ifdef DEBUG_BG95_HIGH
	log("<< " + String((char*)command));
	#endif

	for (uint16_t i = 0; i < size; i++) {
		modem->write(command[i]);
	}
	modem->flush();

}

String MODEMBGXX::get_command(String command, uint32_t timeout){

	send_command(command);
	delay(AT_WAIT_RESPONSE);

	String data = "";
	timeout += millis();
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);
			response.trim();

			if (response.length() == 0) continue; // garbage

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			data += parse_command_line(response);

			//if(response.indexOf("OK") > -1 && data.length() > 0)
			if(response.indexOf("OK") > -1)
				return data;

			if(response.indexOf("ERROR") > -1)
				return "ERROR";
		}

		delay(AT_WAIT_RESPONSE);
	}

	return data;
}

// filters the rcv response and waits for OK to validate it
String MODEMBGXX::get_command(String command, String filter, uint32_t timeout){

	send_command(command);
	delay(AT_WAIT_RESPONSE);

	String data = "";
	timeout += millis();
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue; // garbage

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if(response.startsWith(filter))
				data = response.substring(filter.length());

			/*
			if(response.indexOf("OK") > -1)
				return data;
			*/
			if(response.indexOf("ERROR") > -1)
				return "";
		}else{
			if(data.length() > 0)
				return data;
		}

		delay(AT_WAIT_RESPONSE);
	}

	return data;
}

// filters the rcv response and waits for OK to validate it
String MODEMBGXX::get_command_critical(String command, String filter, uint32_t timeout){

	send_command(command);
	delay(AT_WAIT_RESPONSE);

	String data = "";
	timeout += millis();
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue; // garbage

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			//parse_command_line(response);

			if(response.startsWith(filter))
				data = response.substring(filter.length());

			if(response.indexOf("OK") > -1 && data.length() > 0)
				return data;

			if(response.indexOf("ERROR") > -1)
				return "";
		}else{
			if(data.length() > 0)
				return data;
		}

		delay(AT_WAIT_RESPONSE);
	}

	return data;
}

// filters the rcv response
String MODEMBGXX::get_command_no_ok(String command, String filter, uint32_t timeout){

	send_command(command);
	delay(AT_WAIT_RESPONSE);

	String data = "";
	timeout += millis();
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue; // garbage

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if(response.startsWith(filter)){
				data = response.substring(filter.length());
				return data;
			}

			if(response.indexOf("ERROR") > -1)
				return "";
		}

		delay(AT_WAIT_RESPONSE);
	}

	return data;
}

// filters the rcv response
String MODEMBGXX::get_command_no_ok_critical(String command, String filter, uint32_t timeout){

	send_command(command);
	delay(AT_WAIT_RESPONSE);

	String data = "";
	timeout += millis();
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue; // garbage

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			response = parse_command_line(response,true);

			if(response.startsWith("+QMTRECV:")) // parse MQTT received messages
				mqtt_message_received(response);
			else if(response.startsWith(filter)){
				data = response.substring(filter.length());
				return data;
			}

			if(response.indexOf("ERROR") > -1)
				return "";
		}

		delay(AT_WAIT_RESPONSE);
	}

	return data;
}

// wait for at command status
bool MODEMBGXX::wait_command(String filter, uint32_t timeout){

	delay(AT_WAIT_RESPONSE);

	String data = "";
	timeout += millis();
	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue; // garbage

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if(response.startsWith(filter)){
				return true;
			}

		}

		delay(AT_WAIT_RESPONSE);
	}

	return false;
}

// use it when OK comes in the end
bool MODEMBGXX::check_command(String command, String ok_result, uint32_t wait) {
	bool response_expected = false;
	send_command(command);
	delay(AT_WAIT_RESPONSE);

	uint32_t timeout = millis() + wait;

	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue;

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if (response == ok_result) {
				response_expected = true;
				//break;
			}

			if (response == "ERROR") {
				break;
			}

			if (response.indexOf("+CME ERROR") > -1)
				break;

			if (response.indexOf("OK") > -1)
				break;

		}

		delay(AT_WAIT_RESPONSE);
	}
	/*
	if(response_expected){
		// check if there is an ok in the end of sentence
		String response = modem->readStringUntil(AT_TERMINATOR);
		response.trim();
		#ifdef DEBUG_BG95_HIGH
			log("<< " +response);
		#endif
	}
	*/
	return response_expected;
}

// use it when OK comes in the end
bool MODEMBGXX::check_command(String command, String ok_result, String error_result, uint32_t wait) {
	bool response_expected = false;
	send_command(command);
	delay(AT_WAIT_RESPONSE);

	uint32_t timeout = millis() + wait;

	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue;

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if (response == ok_result) {
				response_expected = true;
			}

			if (response == error_result) {
				break;
			}

			if (response.indexOf("+CME ERROR") > -1)
				break;

			if (response.indexOf("OK") > -1)
				break;

		}

		delay(AT_WAIT_RESPONSE);
	}

	return response_expected;
}

// use it when OK comes before the ok_result
bool MODEMBGXX::check_command_no_ok(String command, String ok_result, uint32_t wait) {
	bool response_expected = false;
	send_command(command);
	delay(AT_WAIT_RESPONSE);

	uint32_t timeout = millis() + wait;

	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue;

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if (response == ok_result) {
				response_expected = true;
				break;
			}

			if (response == "ERROR") {
				break;
			}

			if (response.indexOf("+CME ERROR") > -1)
				break;

		}

		delay(AT_WAIT_RESPONSE);
	}

	return response_expected;
}

// use it when OK comes before the ok_result
bool MODEMBGXX::check_command_no_ok(String command, String ok_result, String error_result, uint32_t wait) {
	bool response_expected = false;
	send_command(command);
	delay(AT_WAIT_RESPONSE);

	uint32_t timeout = millis() + wait;

	while (timeout >= millis()) {
		if (modem->available()) {
			String response = modem->readStringUntil(AT_TERMINATOR);

			response.trim();

			if (response.length() == 0) continue;

			#ifdef DEBUG_BG95_HIGH
				log("<< " +response);
			#endif

			parse_command_line(response);

			if (response == ok_result) {
				response_expected = true;
				break;
			}

			if (response == error_result) {
				break;
			}

			if (response.indexOf("+CME ERROR") > -1)
				break;

		}

		delay(AT_WAIT_RESPONSE);
	}

	return response_expected;
}

void MODEMBGXX::check_commands() {

	if (modem->available()) {
		String response = modem->readStringUntil(AT_TERMINATOR);

		response.trim();

		#ifdef DEBUG_BG95_HIGH
			log("<< " +response);
		#endif

		parse_command_line(response);
	}

}

void MODEMBGXX::log(String text) {
	log_output->println("[" + date() + "] bgxx: " + text);
}

String MODEMBGXX::date() {
	return String(year()) + "-" + pad2(month()) + "-" + pad2(day()) + " " + pad2(hour()) + ":" + pad2(minute()) + ":" + pad2(second());
}

String MODEMBGXX::pad2(int value) {
	return String(value < 10 ? "0" : "") + String(value);
}

boolean MODEMBGXX::isNumeric(String str) {
    unsigned int StringLength = str.length();

    if (StringLength == 0) {
        return false;
    }

    boolean seenDecimal = false;

    for(unsigned int i = 0; i < StringLength; ++i) {
        if (isDigit(str.charAt(i))) {
            continue;
        }

        if (str.charAt(i) == '.') {
            if (seenDecimal) {
                return false;
            }
            seenDecimal = true;
            continue;
        }

				if (str.charAt(i) == '-')
            continue;

        return false;
    }
    return true;
}