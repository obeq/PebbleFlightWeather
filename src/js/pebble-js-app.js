// http://www.met.tamu.edu/class/metar/metar-pg10-sky.html
// https://ww8.fltplan.com/AreaForecast/abbreviations.htm
// http://en.wikipedia.org/wiki/METAR
// http://www.unc.edu/~haines/metar.html

var CLOUDS = {
    NCD: "no clouds",
    SKC: "sky clear",
    CLR: "no clouds under 12,000 ft",
    NSC: "no significant",
    FEW: "few",
    SCT: "scattered",
    BKN: "broken",
    OVC: "overcast",
    VV: "vertical visibility"
};


var WEATHER = {
    // Intensity
    "-": "light intensity",
    "+": "heavy intensity",
    VC: "in the vicinity",

    // Descriptor
    MI: "shallow",
    PR: "partial",
    BC: "patches",
    DR: "low drifting",
    BL: "blowing",
    SH: "showers",
    TS: "thunderstorm",
    FZ: "freezing",

    // Precipitation
    RA: "rain",
    DZ: "drizzle",
    SN: "snow",
    SG: "snow grains",
    IC: "ice crystals",
    PL: "ice pellets",
    GR: "hail",
    GS: "small hail",
    UP: "unknown precipitation",

    // Obscuration
    FG: "fog",
    VA: "volcanic ash",
    BR: "mist",
    HZ: "haze",
    DU: "widespread dust",
    FU: "smoke",
    SA: "sand",
    PY: "spray",

    // Other
    SQ: "squall",
    PO: "dust or sand whirls",
    DS: "duststorm",
    SS: "sandstorm",
    FC: "funnel cloud"
};

function parseAbbreviation(s, map) {
    var abbreviation, meaning, length = 3;
    if (!s) return;
    while (length && !meaning) {
        abbreviation = s.slice(0, length);
        meaning = map[abbreviation];
        length--;
    }
    if (meaning) {
        return {
            abbreviation: abbreviation,
            meaning: meaning
        };
    }
}

function asInt(s) {
    return parseInt(s, 10);
}



function METAR(metarString) {
    this.fields = metarString.split(" ").map(function(f) {
        return f.trim();
    }).filter(function(f) {
        return !!f;
    });
    this.i = -1;
    this.current = null;
    this.result = {};
}

METAR.prototype.next = function() {
    this.i++;
    this.current = this.fields[this.i];
    return this.current;
};

METAR.prototype.peek = function() {
    return this.fields[this.i+1];
};

METAR.prototype.parseStation = function() {
    this.next();
    this.result.station = this.current;
};

METAR.prototype.parseDate = function() {
    this.next();
    var d = new Date();
    d.setUTCDate(asInt(this.current.slice(0,2)));
    d.setUTCHours(asInt(this.current.slice(2,4)));
    d.setUTCMinutes(asInt(this.current.slice(4,6)));
    d.setUTCSeconds(0);
    this.result.time = d;
};

METAR.prototype.parseAuto = function() {
    this.result.auto = this.peek() === "AUTO";
    if (this.result.auto) this.next();
};

var variableWind = /^([0-9]{3})V([0-9]{3})$/;
METAR.prototype.parseWind = function() {
    this.next();
    this.result.wind = {
        speed: null,
        gust: null,
        direction: null,
        variation: null
    };

    var direction = this.current.slice(0,3);
    if (direction === "VRB") {
        this.result.wind.direction = "VRB";
        this.result.wind.variation = true;
    }
    else {
        this.result.wind.direction = asInt(direction);
    }

    var gust = this.current.slice(5,8);
    if (gust[0] === "G") {
        this.result.wind.gust = asInt(gust.slice(1));
    }

    this.result.wind.speed = asInt(this.current.slice(3,5));

    var unitMatch = this.current.match(/KT|MPS|KPH$/);
    if (unitMatch) {
        this.result.wind.unit = unitMatch[0];
    }
    else {
        throw new Error("Bad wind unit: " + this.current);
    }

    var varMatch = this.peek().match(variableWind);
    if (varMatch) {
        this.next();
        this.result.wind.variation = {
            min: asInt(varMatch[1]),
            max: asInt(varMatch[2])
        };
    }
};


METAR.prototype.parseCavok = function() {
    this.result.cavok = this.peek() === "CAVOK";
    if (this.result.cavok) this.next();
};

METAR.prototype.parseVisibility = function() {
    this.result.visibility = null;
    this.result.statuevisibility = null;
    if (this.result.cavok) return;
    this.next();
    if (this.current === "////") return;

    var metricvis = /\d{4}/.exec(this.current);
    if (metricvis) {
        //Visibility in meters
        this.result.visibility = asInt(metricvis);
        return;
    }

    var meters = 0;
    if (this.current.match(/SM/)) {
        //1 word statue mile number, i.e. '2SM'
        this.result.statuevisibility = this.current;
    } else if (this.peek().match(/SM/)) {
        //2 word statue mile number, i.e. '1 1/2SM'
        this.result.statuevisibility = this.current + ' ' + this.peek();
        meters += asInt(this.current) * 1609;
        this.next();
    } else {
        return;
    }

    var smplace = this.current.indexOf("SM");
    var divplace = this.current.indexOf("/");
    if (divplace != -1) {
        //Fraction
        var nom = asInt(this.current.slice(0,divplace));
        var den = asInt(this.current.slice(divplace + 1, smplace));
        meters += nom * 1609 / den;
    } else {
        meters += asInt(this.current.slice(0,smplace)) * 1609;
    }

    this.result.visibility = meters;
    // TODO: Direction too. I've not seen it in finnish METARs...
};

METAR.prototype.parseRunwayVisibility = function() {
    if (this.result.cavok) return;
    while (this.peek().match(/^R[0-9]+/)) {
        this.next();
        // TODO: Parse it. I've not seen it in finnish METARs...
    }
};



function parseWeatherAbbrv(s, res) {
    var weather = parseAbbreviation(s, WEATHER);
    if (weather) {
        res = res || [];
        res.push(weather);
        return parseWeatherAbbrv(s.slice(weather.abbreviation.length), res);
    }
    return res;
}

METAR.prototype.parseWeather = function() {
    this.result.weather = [];
    if (this.result.cavok) return;
    while (true) {
      var weather = parseWeatherAbbrv(this.peek());
      if (!weather) break;
      this.result.weather.push(weather);
      this.next();
    }
};


METAR.prototype.parseClouds = function() {
    if (!this.result.clouds) this.result.clouds = null;
    if (this.result.cavok) return;
    var cloud = parseAbbreviation(this.peek(), CLOUDS);
    if (!cloud) return;

    this.next();

    cloud.altitude = asInt(this.current.slice(cloud.abbreviation.length))*100 || null;
    cloud.cumulonimbus = /CB$/.test(this.current);

    this.result.clouds = (this.result.clouds || []);
    this.result.clouds.push(cloud);

    this.parseClouds();
};

METAR.prototype.parse = function() {
    this.parseStation();
    this.parseDate();
    this.parseAuto();
    this.parseWind();
    this.parseCavok();
    this.parseVisibility();
    this.parseRunwayVisibility();
    this.parseWeather();
    this.parseClouds();
};



  function parseMETAR(metarString) {
    var m = new METAR(metarString);
    m.parse();
    return m.result;
}

var messageQueue = [];
var currentMessage = null;
var MAX_RETRIES = 3;

var BASIC_CONFIG = { 'largefont' : false, 'battery' : false, 'location' : true};
var configuration = BASIC_CONFIG;
                    
function roughSizeOfObject( object ) {
//Returns the rough size of an object, for message fail debugging.
    var objectList = [];
    var stack = [ object ];
    var bytes = 0;

    while ( stack.length ) {
        var value = stack.pop();

        if ( typeof value === 'boolean' ) {
            bytes += 4;
        }
        else if ( typeof value === 'string' ) {
            bytes += value.length * 2;
        }
        else if ( typeof value === 'number' ) {
            bytes += 8;
        }
        else if
        (
            typeof value === 'object'
            && objectList.indexOf( value ) === -1
        )
        {
            objectList.push( value );

            for( i in value ) {
                stack.push( value[ i ] );
            }
        }
    }
    return bytes;
}

function describe(d) {
//Returns a string describing a dictionary type object, for debugging.
  return JSON.stringify(d);
}

//Messaging functions. Messages are placed in a send queue by sendMessage, who then calls doSend.
//If no send is in progress, doSend sends the next message in the send queue. Upon successful delievery
//doSend is called again, sending the next message in the queue. Upon failed delievery, the failure is logged.
//doSend is then called again, effectively dropping the failed message. Delievery is thus not guaranteed at this
//point.

function sendSuccess(e) {
//Called upon successful delievery of a message.
  if ((currentMessage) && (currentMessage.mid == e.data.transactionId)) {
    //console.log("Message with id " + e.data.transactionId + " was sent successfully.");
  } else {
    if (!currentMessage) {
      console.log("Error! Currentmessage not set!");
    } else {
      console.log("Error! Message with id " + e.data.transactionId + " was sent, but id " + currentMessage.mid + " was excpected.");
    }
  }
  currentMessage = null;
  doSend();
}

function sendFail(e) {
//Called upon failed delievery of a message. Message is dropped.
  console.log("Message with id " + e.data.transactionId + " failed! Error: " + e.data.error.message);
  if (currentMessage.retries) {
    currentMessage.retries--;
    messageQueue.push(currentMessage);
  }
  currentMessage = null;
  doSend();
}

function doSend() {
//If there are messages in the queue, and if no message is currently being sent, send the next message.
  if (messageQueue.length > 0) {
    if (!currentMessage) {
      var message = messageQueue.shift();
         
      message.mid = Pebble.sendAppMessage(message.text, sendSuccess, sendFail); //For some reason, Pebble seems to return a msgid that is the sent messages reported id minus 1.
      currentMessage = message;
      //console.log("Sending message with id " + currentMessage + ".");
      //console.log("Estimated size of message: " + roughSizeOfObject(message));
    }
  }
}

function sendMessage(s) {
//Places s in the message queue, and calls doSend to commence sending.
  console.log("Enqueueing message to pebble: " + describe(s));
  
  var message = {};
  message.text = s;
  message.retries = MAX_RETRIES;

  messageQueue.push(message);
  doSend();
}

function updateLocation() {
//Initiates location progress.
  if (configuration.location) {
    sendMessage({'location': 1});
    window.navigator.geolocation.getCurrentPosition(locationSuccess, locationError, {"timeout": 60000, "maximumAge": 15 * 60 * 1000 });
  } else {
    sendMessage({'location': -1, 'station': configuration.station});
  }
}

function fetchWeb(url) {
  //Accepts either an url as a string or an array of urls. Returns a request for the first url that returns with a 200 code, i.e. success.
  //If no urls result in a 200 code, the request for the last url is returned. All web requests are done synchronously.

  sendMessage({'net': 1});
  var urls = [];
  var req = new XMLHttpRequest();
  
  if (typeof(url) === 'string') {
    urls = [url];
  } else {
    urls = url;
    urls.reverse();
  }

  do {
    url = urls.pop();
    console.log("Web request for url: " + url);
    req.open('GET', url, false);
    req.send(null);
  } while ((req.status != 200) && urls);

  sendMessage({'net': 0});
  return req;
}

function fetchMetar(station) {
//Fetches metar for a given station.
  var response;
  var raw_text;
  var city;
  var metar;

  var urls = [
    'http://olofbeckman.se/metar/station/' + station.toUpperCase(),
    'http://weather.noaa.gov/pub/data/observations/metar/stations/' + station.toUpperCase() + '.TXT'
  ];

  var req = fetchWeb(urls);

  if (req.status == 200) {
    //The return is just a two line text file, where the first line is a timestamp. The second line is the metar. I should probably check for validity at this point. TODO.
    raw_text = req.responseText; //.split("\n")[1]; 
    var d = new Date();
    var hours = d.getHours();
    var minutes = d.getMinutes();

    var lowestCloud = -1;
    var lowestCloudType = "";
    var message = "";

    metar = parseMETAR(raw_text);
    if (metar.clouds) {
      //Cycle through all the clouds and find which are at the lowest level. Save the type of that cloud.
      metar.clouds.forEach(function(entry) {
        if ((entry.abbreviation != 'VV') && (entry.altitude) && ((lowestCloud < 0) || (entry.altitude < lowestCloud))) {
          lowestCloud = entry.altitude;
          lowestCloudType = entry.meaning;
        }
      });
    }    

    //console.log("Lowest altitude of clouds found were: " + lowestCloud);

    if ((lowestCloud > -1) && (lowestCloud < 1500)) {
      //If clouds are found below 1500 feet we have IMC (not really though, they have to be of type OVC or BKN which is a TODO.)
      message = "There are " + lowestCloudType + " clouds at " + lowestCloud + " feet\n";
    }

    if ((metar.visibility) && (metar.visibility < 5000)) {
      //If visibility is lower than 5000 meters, we have IMC.
      if (metar.statuevisibility) {
        message += "Visibility: " + metar.statuevisibility;
      } else {
        message += "Visibility: " + metar.visibility + "m";
      }
    }

    //Yes, visibility is measured in meters and cloud height in feet. Flying is a standards nightmare.
    
    //var seconds_ago = Math.round(d.getTime() - metar.time.getTime()) / 1000;
    console.log(d.getTimezoneOffset());
    var seconds_ago = Math.round(metar.time.getTime() / 1000 - d.getTimezoneOffset() * 60);

    if (message) {
      //If message is set, we have IMC conditions and report those. Watchapp will assume VMC if clouds is not set.
      sendMessage({"updated": seconds_ago, "metar": raw_text, "clouds": message});
    } else {
      //If message is not set, we have VMC conditions.
      sendMessage({"updated": seconds_ago, "metar": raw_text});
    }

    
    //city = hours + ':' + (minutes < 10 ? "0" : "") + minutes;        
    //console.log(city + " - " + raw_text);
    
  } else {
    //Web request unsuccessful. Reported by setting 'net' to zero.
    console.log("Metar check failed with error " + req.status);
    sendMessage({"net": 0});
  }
}

function locationSuccess(pos) {
//Called on successful location lock. Requests the metar of the closest airport from geonames, giving us the 
//station name of the closest airport. However, geonames updates the Metars slowly and sometimes gives an 
//older, outdated metar which is why we're not using the actual metar text from geonames.

  var latitude = pos.coords.latitude;
  var longitude = pos.coords.longitude;
    //console.log("Got position: " + latitude + "/" + longitude); //Don't log this on published app, for privacy reasons.

  var response;
  var raw_text;
  var metar;
  var req = fetchWeb('http://api.geonames.org/findNearByWeatherJSON?lat=' + latitude + '&lng=' + longitude + '&radius=1000&username=olofbeckman');

  if (req.status == 200) {
    //I should do some validation here as well. TODO
    response = JSON.parse(req.responseText);
    raw_text = response.weatherObservation.observation;
    metar = parseMETAR(raw_text);
    if (metar.station) {
      //console.log("Closest station: " + metar.station);
      sendMessage({"station": metar.station});
    }
  } else {
    console.log("Geonames failed with error " + req.status);
    sendMessage({"net": 0});
  }
  sendMessage({"location": 0}); //Report to watch that location lookup has finished.
}

function locationError(err) {
//On failed location lookup. Report unsuccessful location to watch.
  console.log("Error getting location.");
  sendMessage({"location": -1, "station": configuration.station}); //Report to watch that location lookup has finished.
}

function loadConfig() {
  var configjson = localStorage.config;
                                                                                                                                                                                                                                                                                                                                                                                                                             
  if (configjson) {
    configuration = JSON.parse(configjson);
  } 
  if (!configuration) {
    configuration = BASIC_CONFIG;
  }
}

Pebble.addEventListener("ready",
  function(e) {
    console.log("Connected to Pebble. " + e.ready);
    loadConfig();
  });

//There are currently three requests possible:

//Location: Returns the name of the closest station.
//Metar: Returns the metar for a given station.
//Init: Returns a init message to show that the js is running.

Pebble.addEventListener("appmessage",
  function(e) {
    console.log("Got message from Pebble: " + describe(e.payload));
    if (e.payload.request) {
      loadConfig();
      if (e.payload.request == "location") {
        updateLocation();
      }
      if ((e.payload.request == "metar") && (e.payload.station)) {
        fetchMetar(e.payload.station);
        configuration.station = e.payload.station;
        localStorage.setItem("config", JSON.stringify(configuration));
      }
      if (e.payload.request == "init") {
        var bat_save = configuration.battery ? 1 : 0;
        var largefont = configuration.largefont ? 1 : 0;
        var seconds = configuration.seconds ? 1 : 0;
        sendMessage({"init": 1, "seconds": seconds, "bat": bat_save, "largefont": largefont});
      }
    }
  }
);

Pebble.addEventListener("showConfiguration",
  function(e) {
    var batstr = configuration.battery ? 'true' : 'false';
    var gpsstr = configuration.location ? 'true' : 'false';
    var fontstr = configuration.largefont ? 'true' : 'false';
    var secstr = configuration.seconds ? 'true' : 'false';
    var stationstr = configuration.station;
    Pebble.openURL("http://olofbeckman.se/config/application/metarconfig?version=4&seconds="+secstr+"&battery="+batstr+"&location="+gpsstr+"&station="+stationstr+"&largefont="+fontstr);
  }
);

Pebble.addEventListener("webviewclosed",
  function(e) {
    //console.log("Configuration returned: " + e.response);
    if (e.response) {
      var configjson = decodeURIComponent(e.response);
      var config_before = configuration;
      configuration = JSON.parse(configjson);
      //console.log("Configuration window returned: ", JSON.stringify(configuration));
      localStorage.setItem("config", configjson);

      if (config_before.location != configuration.location) {      //If location has changed
        if (configuration.location) {
          updateLocation();
        } else {
          sendMessage({'location': 0});
          if (configuration.station) {
            sendMessage({'station': configuration.station});
          }
        }
      } else {
        if ((!configuration.location) && (configuration.station !== "") && (config_before.station != configuration.station)) {
          sendMessage({'station': configuration.station});
        }
      }

      if (config_before.battery != configuration.battery) {
        var bat_save = configuration.battery ? 1 : 0;
        sendMessage({'init': 1, 'bat': bat_save});
      }
      
      if (config_before.seconds != configuration.seconds) {
        var seconds = configuration.seconds ? 1 : 0;
        sendMessage({'init': 1, 'seconds': seconds});
      }

      if (config_before.largefont != configuration.largefont) {
        var largefont = configuration.largefont ? 1 : 0;
        sendMessage({'largefont': largefont});
      }
    }
  }
);