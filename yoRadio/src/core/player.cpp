#include "options.h"

#include "player.h"

#include "config.h"
#include "telnet.h"
#include "display.h"

#include "netserver.h"

Player player;

#if VS1053_CS!=255 && !I2S_INTERNAL
  #if VS_HSPI
    Player::Player(): Audio(VS1053_CS, VS1053_DCS, VS1053_DREQ, HSPI, 13, 12, 14) {}
  #else
    Player::Player(): Audio(VS1053_CS, VS1053_DCS, VS1053_DREQ) {}
  #endif
  void ResetChip(){
    pinMode(VS1053_RST, OUTPUT);
    digitalWrite(VS1053_RST, LOW);
    delay(30);
    digitalWrite(VS1053_RST, HIGH);
    delay(100);
  }
#else
  #if !I2S_INTERNAL
    Player::Player() {}
  #else
    Player::Player(): Audio(true, I2S_DAC_CHANNEL_BOTH_EN)  {}
  #endif
#endif


void Player::init() {
  Serial.print("##[BOOT]#\tplayer.init\t");
#ifdef MQTT_ROOT_TOPIC
  memset(burl, 0, 400);
#endif
  if(MUTE_PIN!=255) pinMode(MUTE_PIN, OUTPUT);
  #if I2S_DOUT!=255
    #if !I2S_INTERNAL
      setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    #endif
  #else
    SPI.begin();
    if(VS1053_RST>0) ResetChip();
    begin();
  #endif
  setBalance(config.store.balance);
  setTone(config.store.bass, config.store.middle, config.store.trebble);
  setVolume(0);
  mode = STOPPED;
  setOutputPins(false);
  requestToStart = true;
  volTimer=false;
  zeroRequest();
  playmutex = xSemaphoreCreateMutex();
  randomSeed(analogRead(0));
  Serial.println("done");
}

void Player::stopInfo() {
  config.setSmartStart(0);
  telnet.info();
  netserver.requestOnChange(MODE, 0);
  requestToStart = true;
}

void Player::stop(const char *nttl){
  if(config.store.play_mode==PM_SDCARD) config.sdResumePos = player.getFilePos();
  mode = STOPPED;
  setOutputPins(false);
  if(nttl) config.setTitle(nttl);
  else config.setTitle((display.mode()==LOST || display.mode()==UPDATING)?"":const_PlStopped);
  netserver.requestOnChange(TITLE, 0);
  config.station.bitrate = 0;
  #ifdef USE_NEXTION
    nextion.bitrate(config.station.bitrate);
  #endif
  netserver.requestOnChange(BITRATE, 0);
  display.putRequest(DBITRATE);
  display.putRequest(PSTOP);
  //setDefaults();
  stopSong();
  stopInfo();
  if (player_on_stop_play) player_on_stop_play();
}

void Player::initHeaders(const char *file) {
  if(strlen(file)==0) return;
  connecttoFS(SD,file);
  eofHeader = false;
  //for(int c=0;c<20;c++) player.loopreader();
  while(!eofHeader) Audio::loop();
  //netserver.requestOnChange(SDPOS, 0);
  setDefaults();
}

void Player::loop() {
  if (mode == PLAYING) {
    xSemaphoreTake(playmutex, portMAX_DELAY);
    Audio::loop();
    xSemaphoreGive(playmutex);
  } else {
    if (isRunning())  stop();
  }
  if (request.station > 0) {
    if (request.doSave) {
      config.setLastStation(request.station);
    }
    play(request.station);
    if (player_on_station_change) player_on_station_change();
    zeroRequest();
  }
  if (request.volume >= 0) {
    config.setVolume(request.volume);
    telnet.printf("##CLI.VOL#: %d\n", config.store.volume);
    Audio::setVolume(volToI2S(request.volume));
    zeroRequest();
    display.putRequest(DRAWVOL);
    netserver.requestOnChange(VOLUME, 0);
  }
  if(volTimer){
    if((millis()-volTicks)>3000){
      config.saveVolume();
      volTimer=false;
    }
  }
#ifdef MQTT_ROOT_TOPIC
  if(strlen(burl)>0){
    browseUrl();
  }
#endif
}

void Player::zeroRequest() {
  request.station = 0;
  request.volume = -1;
  request.doSave = false;
}

void Player::setOutputPins(bool isPlaying) {
  if(LED_BUILTIN!=255) digitalWrite(LED_BUILTIN, LED_INVERT?!isPlaying:isPlaying);
  if(MUTE_PIN!=255) digitalWrite(MUTE_PIN, isPlaying?!MUTE_VAL:MUTE_VAL);
}

void Player::play(uint16_t stationId, uint32_t filePos) {
  remoteStationName = false;
  config.setDspOn(1);
  display.putRequest(PSTOP);
  setDefaults();
  setOutputPins(false);
  config.setTitle(config.store.play_mode==PM_WEB?const_PlConnect:"");
  config.station.bitrate=0;
  netserver.requestOnChange(TITLE, 0);
  config.loadStation(stationId);
  setVol(config.store.volume, true);
  display.putRequest(NEWSTATION);
  netserver.requestOnChange(STATION, 0);
  telnet.printf("##CLI.NAMESET#: %d %s\n", config.store.lastStation, config.station.name);
  if (config.store.play_mode==PM_WEB?connecttohost(config.station.url):connecttoFS(SD,config.station.url,config.sdResumePos==0?filePos:config.sdResumePos-player.sd_min)) {
    mode = PLAYING;
    if(config.store.play_mode==PM_SDCARD) config.sdResumePos = 0;
    config.setTitle("");
    netserver.requestOnChange(TITLE, 0);
    config.setSmartStart(1);
    netserver.requestOnChange(MODE, 0);
    setOutputPins(true);
    requestToStart = true;
    display.putRequest(PSTART);
    if (player_on_start_play) player_on_start_play();
  }else{
    telnet.printf("##ERROR#:\tError connecting to %s\n", config.station.url);
  };
}

#ifdef MQTT_ROOT_TOPIC
void Player::browseUrl(){
  remoteStationName = true;
  config.setDspOn(1);
  resumeAfterUrl = mode==PLAYING;
  display.putRequest(PSTOP);
  setDefaults();
  setOutputPins(false);
  config.setTitle(const_PlConnect);
  netserver.requestOnChange(TITLE, 0);
  if (connecttohost(burl)){
    mode = PLAYING;
    config.setTitle("");
    netserver.requestOnChange(TITLE, 0);
    netserver.requestOnChange(MODE, 0);
    setOutputPins(true);
    requestToStart = true;
    display.putRequest(PSTART);
    if (player_on_start_play) player_on_start_play();
  }else{
    telnet.printf("##ERROR#:\tError connecting to %s\n", burl);
  }
  memset(burl, 0, 400);
}
#endif

void Player::prev() {
  if(config.store.play_mode==PM_WEB || !config.sdSnuffle){
    if (config.store.lastStation == 1) config.store.lastStation = config.store.countStation; else config.store.lastStation--;
  }
  request.station = config.store.lastStation;
  request.doSave = true;
}

void Player::next() {
  if(config.store.play_mode==PM_WEB || !config.sdSnuffle){
    if (config.store.lastStation == config.store.countStation) config.store.lastStation = 1; else config.store.lastStation++;
  }else{
    config.store.lastStation = random(1, config.store.countStation);
  }
  request.station = config.store.lastStation;
  request.doSave = true;
}

void Player::toggle() {
  if (mode == PLAYING) {
    mode = STOPPED;
  } else {
    request.station = config.store.lastStation;
  }
}

void Player::stepVol(bool up) {
  if (up) {
    if (config.store.volume <= 254 - config.store.volsteps) {
      setVol(config.store.volume + config.store.volsteps, false);
    }else{
      setVol(254, false);
    }
  } else {
    if (config.store.volume >= config.store.volsteps) {
      setVol(config.store.volume - config.store.volsteps, false);
    }else{
      setVol(0, false);
    }
  }
}

byte Player::volToI2S(byte volume) {
  int vol = map(volume, 0, 254 - config.station.ovol * 3 , 0, 254);
  if (vol > 254) vol = 254;
  if (vol < 0) vol = 0;
  return vol;
}

void Player::setVol(byte volume, bool inside) {
  if (inside) {
    setVolume(volToI2S(volume));
  } else {
    volTicks = millis();
    volTimer = true;
    request.volume = volume;
    request.doSave = true;
  }
}
