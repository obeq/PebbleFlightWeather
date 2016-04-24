/*{{{*/
#include <pebble.h>
#include <string.h>
#include "PDutils.h"

#define MINUTES 60 * 1000

#define MAX_TIME_BETWEEN_UPDATES 70
#define LOCATION_INTERVAL 20

#define HIGH_INTERVAL 1
#define LOW_INTERVAL 14 
#define BASE_INTERVAL 5
#define BAT_SAVE_INTERVAL 60

#define LOW_TRESHOLD 25
#define HIGH_TRESHOLD 37 

#define TEXT_LAYER_Y 78

#define LAYER_TIMERS 10

#define SCROLL_INTERVAL 10 * 1000
/*}}}*/

//Data structures {{{
typedef struct {
    Layer *layer;
    AppTimer *timer;
} LayerTimer;
// }}}

//UI elements {{{

static Window *window;

static TextLayer *weather_layer;
static Layer *weather_layer_frame;             // The frame in which the weather layer resides. For clipping.
static PropertyAnimation *weather_animation;

static TextLayer *clock_layer;
static TextLayer *date_layer;
static TextLayer *metar_age_layer;
// }}}

//The status layer and its icons. {{{
static Layer *status_layer;

//static Layer *text_frame_layer;
static Layer *status_layer;

static BitmapLayer *bt_icon_layer;
static GBitmap *bt_icon;

static BitmapLayer *conn_icon_layer;
static GBitmap *conn_icon;

static BitmapLayer *net_icon_layer;
static GBitmap *net_icon;

static BitmapLayer *gps_icon_layer;
static GBitmap *gps_icon;

static BitmapLayer *imc_icon_layer;
static GBitmap *imc_icon;

// }}}

// The dialog layer {{{
static Layer *dialog_layer;
char* dialog_message = NULL;
char* dialog_title = NULL;
// }}}

//Timers {{{

static AppTimer *requestWatchMetar;              // For checking that the phone responds in time.
static AppTimer *requestWatchLocation;              // For checking that the phone responds in time.
static AppTimer *requestWatchInit;              // For checking that the phone responds in time.
static AppTimer *textAnimationTimer;        // Times scrolling of the metar text field.

static LayerTimer *layer_timers;
// }}}


//Data variables {{{

//Saved timestamps for update cycle. {{{
static time_t last_weather_update = 0;
static time_t last_weather_check = 0;
static time_t last_location = 0;
static time_t metar_update_time = 0;
// }}}

//Weather and station {{{
static char *station = NULL;
static char *metar = NULL;
bool imc = false;
int initial = 2;
// }}}

//Status {{{
static bool bt_connected = true;
static bool app_connected = false;
// }}}

//Settings {{{
static bool setting_bat_save = false;
static bool setting_largefont = false;
static bool setting_seconds = true;
// }}}
// }}}

//Function declarations
void doScroll(void *);

//Keys for app message {{{
enum {
    METAR_KEY = 0x0,
    REQUEST_KEY = 0x1,
    STATION_KEY = 0x2,
    STATUS_KEY = 0x3,
    INIT_KEY = 0x4,
    LOCATION_KEY = 0x5,
    NET_KEY = 0x6,
    CLOUDS_KEY = 0x7,
    BAT_KEY = 0x8,
    LARGEFONT_KEY = 0x9,
    SECONDS_KEY = 0xa,
    UPDATED_KEY = 0xb
};

// }}}

//Metar text field animation logic {{{

void scroll_animation_started(Animation *animation, void *data) {
    //Called when the scrolling of the metar text field has started. No-op currently.
}

void scroll_animation_stopped(Animation *animation, bool finished, void *data) {
    /* 
    Called when the scrolling of the metar text field has stopped. 
    Schedules a new scroll in SCROLL_INTERVAL seconds.
    */
    if (textAnimationTimer) {
        app_timer_cancel(textAnimationTimer);
        textAnimationTimer = NULL;
    }
    textAnimationTimer = app_timer_register(SCROLL_INTERVAL, doScroll, NULL);
}

void scrollTextLayer(int distance) {
    /*
       Initiates a scroll of the metar text field by distance pixels. Positive value of distance scrolls the
       text field downwards on screen.
       */
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "A scroll of the weather field by %d has been requested.", distance);
    GRect from_frame = layer_get_frame((Layer *)weather_layer);
    GRect to_frame = (GRect) { .origin = { from_frame.origin.x, from_frame.origin.y + distance }, .size = from_frame.size };

    property_animation_destroy(weather_animation);

    weather_animation = property_animation_create_layer_frame((Layer *) weather_layer, &from_frame, &to_frame);
    animation_set_curve((Animation *) weather_animation, AnimationCurveEaseInOut);
    animation_set_duration((Animation *) weather_animation, 2000);

    animation_set_handlers((Animation*) weather_animation, (AnimationHandlers) {
                .started = (AnimationStartedHandler) scroll_animation_started,
                .stopped = (AnimationStoppedHandler) scroll_animation_stopped,
            }, NULL);

    animation_schedule((Animation*) weather_animation);
}

void doScroll(void *data) {
    /*
       Scrolls the metar text field either so that bottom part of field is visible or back to start if it already
       has been scrolled.
       *data is ignored.
       */
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "A scroll of the weather field has been requested.");
    GRect text_layer_frame = layer_get_frame((Layer *) weather_layer);

    //Check if layer has already been scrolled.
    if (text_layer_frame.origin.y != -4) {
        //Scroll back
        scrollTextLayer(-text_layer_frame.origin.y - 4);
    } else {
        //Check if it needs to be scrolled. If it is bigger than 87 then it needs to be solved. TODO: Remove the
        //magic number.
        GSize est_size = text_layer_get_content_size(weather_layer);
        if (est_size.h > 72) {
            scrollTextLayer(72 - est_size.h);
        }
    }
}

void resetScrolling() {
    /*
       Scrolls the metar text field back to starting position, if it's not already there. Initiates a scrolling if needed as usual.
       */
    if (textAnimationTimer) {
        app_timer_cancel(textAnimationTimer);
        textAnimationTimer = NULL;
    }
    doScroll(NULL);
}

// }}}

//Various show and hide functions {{{

void cancelTimer(Layer *layer) {
    /*
       Cancels a previously set hide timer for the layer, if set.
       */
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Cancelling a timer.");
    for (int i = 0; i < LAYER_TIMERS; i++) {
        if (layer_timers[i].layer) {
            if (layer_timers[i].layer == layer) {
                app_timer_cancel(layer_timers[i].timer);
                layer_timers[i].layer = NULL;
                break;
            }
        }
    }
}

void setTimer(Layer *layer, uint32_t timeout, AppTimerCallback callback, void *callback_data) {
    /*
       Sets a timer with the specified callback and data, and associates it with a layer. The timer can then be
       cancelled by calling cancelTimer with the same layer as argument. Any existing timer for that layer is
       cancelled.
       */
    
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Setting a timer.");
    cancelTimer(layer);
    
    AppTimer *new_timer = app_timer_register(timeout, callback, callback_data);
    for (int i = 0; i < LAYER_TIMERS; i++) {
        if (layer_timers[i].layer == NULL) {
            layer_timers[i].layer = layer;
            layer_timers[i].timer = new_timer;
            break;
        }
    }
}


void hideLayer(void *data) {
    /*
       Hides the layer. data will be casted to a layer, which will be hidden.
       */
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Hiding a layer.");
    Layer *layer = (Layer *) data;
    layer_set_hidden(layer, true);
}

void hideLayerDelayed(Layer *layer, uint32_t timeout) {
    /*
       Hides the layer in timeout milliseconds.
       */
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Enquiing a hide of a layer.");
    setTimer(layer, timeout, hideLayer, layer);
}

void showLayer(Layer *layer) {
    /*
       Shows the layer.
       */
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Showing a layer.");
    layer_set_hidden(layer, false);
}

void showStatus() {
    /*
       Changes visibility of icons in status field depending on the status of various variables.
       */
    layer_set_hidden((Layer *) bt_icon_layer, bluetooth_connection_service_peek()); 
    layer_set_hidden((Layer *) conn_icon_layer, app_connected);
    layer_set_hidden((Layer *) imc_icon_layer, !imc);
}

void setMetarFont() {
    /*
       Resizes the font in Metar text field so that the text fits in the field.
       */
    text_layer_set_font(weather_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    text_layer_set_overflow_mode(weather_layer, GTextOverflowModeWordWrap);
    
    GSize est_size = text_layer_get_content_size(weather_layer);
    //GRect bounds = layer_get_bounds(text_layer_get_layer(weather_layer));

    //if (est_size.h > bounds.size.h) {
    if (est_size.h > 87) {
        if (!setting_largefont) {
            text_layer_set_font(weather_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
            text_layer_set_overflow_mode(weather_layer, GTextOverflowModeFill);
        }
        if (textAnimationTimer) {
        app_timer_cancel(textAnimationTimer);
            textAnimationTimer = NULL;
        }
        textAnimationTimer = app_timer_register(15 * 1000, doScroll, NULL);
    }
}
// }}}

//Dialog box {{{

void update_dialog_layer_callback(Layer *layer, GContext *ctx) {
    /*
       Callback that is called when the dialog layer needs to be (re)drawn).
       */
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx, GColorWhite);

    GRect bounds = layer_get_frame(layer);
    GRect draw_frame = (GRect) { .origin = { 0, 0 }, .size = bounds.size };

    graphics_fill_rect(ctx, draw_frame, 4, GCornersAll);
    //graphics_draw_round_rect(ctx, bounds, 4);

    graphics_draw_text(ctx, dialog_title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), (GRect) { .origin = { 3, 0}, .size = { draw_frame.size.w - 6, draw_frame.size.h } }, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, dialog_message, fonts_get_system_font(FONT_KEY_GOTHIC_18), (GRect) { .origin = { 3, 18}, .size = { draw_frame.size.w - 6, draw_frame.size.h } }, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

//TODO Nicer show dialog function.
// }}}

//UI Events {{{

void watch_tapped(AccelAxisType axis, int32_t direction) {
    /* 
       Called when the user taps the watch. Hides the dialog if visible and resets the scrolling.
       */
    layer_set_hidden(dialog_layer, true);
    resetScrolling();
}

void bluetooth_connection_changed(bool connected) {
    /*
       Called whenever the status of the bluetooth connection has changed.
       */
    showStatus();

    if (!connected) {
      if (bt_connected) {
        vibes_double_pulse();
      }
        //vibes_double_pulse();
        /*if (requestWatch) {
            app_timer_cancel(requestWatch);
            requestWatch = NULL;
        }*/
    }
    bt_connected = connected;
}
// }}}

//Phone communication logic {{{

//Request functions {{{

void requestFailed(void *data) {
    /*
       Called when the phone did not respond in time to a request.
       */
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Phone did not respond in time!");

    app_connected = false;
    showStatus();

    if (data != NULL) {
        void (*callback)(void) = data;
        callback();
    }
}

void initConnection() {
    /*
       Sends an init request to the phone, to (re)initialize the connection. If the javascript app on the phone is
       running, Pebble will start it. The JS will then respond with init and some settings. If the Pebble app is
       not running on the found, there will be no response. This function will retry every 5 seconds.
       */
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);

    Tuplet request = TupletCString(REQUEST_KEY, "init");
    dict_write_tuplet(iter, &request);

    if(requestWatchInit) {
        app_timer_cancel(requestWatchInit);
        requestWatchInit = NULL;
    }

    requestWatchInit = app_timer_register(5 * 1000, requestFailed, &initConnection);
    app_message_outbox_send();
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Init request sent.");
}

bool confirmConnection() {
    bool result = false;

    if (bt_connected) {
        if (app_connected) {
            result = true;
        } else {
            initConnection();
            result = false;
        }
    }

    return result;
}

void requestLocation() {
    /*
       Sends a location request to the phone. If the Pebble app is not running on the phone, there will be no
       response. This function will retry every minute. If the Pebble app is running but the location cant be
       aquired, there will be a 'location': 0 response.
       */
    if (!confirmConnection()) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Phone not connected.");
        return;
    }


    last_location = time(NULL);

    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    
    Tuplet request = TupletCString(REQUEST_KEY, "location");
    dict_write_tuplet(iter, &request);

    if (requestWatchLocation) {
        app_timer_cancel(requestWatchLocation);
        requestWatchLocation = NULL;
    }
    requestWatchLocation = app_timer_register(1 * MINUTES, requestFailed, &initConnection);
    app_message_outbox_send();
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Location request sent.");
}

void requestUpdate() {
    /*
       Sends a request for updated Metar to the phone.
       */
    if (!confirmConnection()) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Phone not connected.");
        return;
    }
    
    //Check if the location has been updated in a while. Otherwise, check that first.
    time_t seconds_now = time(NULL);

    if (!last_location) {
        requestLocation();
        return;
    } else {
        int difference = (seconds_now - last_location) / 60;    

        if ((station == NULL) || (difference > LOCATION_INTERVAL)) {
            requestLocation();
            return;
        } 
    }
    last_weather_check = seconds_now;

    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);

    Tuplet request = TupletCString(REQUEST_KEY, "metar");
    dict_write_tuplet(iter, &request);

    Tuplet station_t = TupletCString(STATION_KEY, station);
    dict_write_tuplet(iter, &station_t);

    if (requestWatchMetar) {
        app_timer_cancel(requestWatchMetar);
        requestWatchMetar = NULL;
    }
    requestWatchMetar = app_timer_register(1 * MINUTES, requestFailed, &initConnection);
    app_message_outbox_send();
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Update request sent.");
}

int calculateInterval() {
    /*
       Calculate the current interval for Metar requests in minutes.
       */
    int result = BASE_INTERVAL;

    time_t seconds_now = time(NULL);
    int time_since_update = (seconds_now - last_weather_update) / 60;

    if (setting_bat_save) {
        result = BAT_SAVE_INTERVAL;
    } else {
        if (initial == 0) {
            if ((time_since_update > LOW_TRESHOLD) && (time_since_update < HIGH_TRESHOLD)) {
				// APP_LOG(APP_LOG_LEVEL_DEBUG, "Entering high intensity polling.");
                result = HIGH_INTERVAL;
            } else {
				// APP_LOG(APP_LOG_LEVEL_DEBUG, "Entering low intensity polling.");
                result = LOW_INTERVAL;
            }
        }
    }

    return result;
}

// }}}

//Handlers {{{

static void handle_minute_tick(struct tm *tick_time, TimeUnits units_changed) {
    /*
       Called every minute. Updates the watch face and requests for location and weather updates when needed.
       */
    
    time_t seconds_now = p_mktime(tick_time);
  
    //Update watch face.
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Minte tick to handle.");
    static char time_text[] = "00:00:00";
    static char date_text[] = "Mon Jan 31 2000";
  
    static char metar_age[] = "Issued more than 4 hours ago.";
    
    int age = (int) (seconds_now - metar_update_time) / 60;
    if (age > 240) {
      snprintf(metar_age, sizeof(metar_age), "Issued more than %d hours ago", 4);
    } else {  
      snprintf(metar_age, sizeof(metar_age), "Issued %ld minutes ago", (seconds_now - metar_update_time) / 60);
    }
  
    if (setting_seconds) {
      strftime(time_text, sizeof(time_text), "%H:%M:%S", tick_time);
      strftime(date_text, sizeof(date_text), "%a %b %d %Y", tick_time);
    } else {
      strftime(time_text, sizeof(time_text), "%H:%M", tick_time);
    }
  
    text_layer_set_text(clock_layer, time_text);
    text_layer_set_text(date_layer, date_text);
    text_layer_set_text(metar_age_layer, metar_age);

    //Request weather update if needed.
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Checking if weather needs to be updated.");
    int difference = (seconds_now - last_weather_check) / 60;
    int interval = calculateInterval();
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "%d minutes have passed since last weather check. Current interval is: %d", difference, interval);

    if (difference >= calculateInterval()) { 
        // APP_LOG(APP_LOG_LEVEL_DEBUG, "%d minutes have passed since last weather check. Requesting update.", 
        //            difference);
        requestUpdate();
        last_weather_check = seconds_now;
    }
}


void out_sent_handler(DictionaryIterator *sent, void *context) {
    /* 
       Called when a message was delievered to phone.
       */

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Update request delievered.");
}

void out_failed_handler(DictionaryIterator *failed, AppMessageResult reason, void *context) {
    /*
       Called when a message to phone failed. We don't much care about this, since we have timers checking that
       the phone actually responds. It would be smarter to implement something here, but it would make the code
       a bit more complicated to follow.
       */
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Update request failed.");
}

/*void cancelRequestTimer() {
    if (requestWatch) {
        app_timer_cancel(requestWatch);
        requestWatch = NULL;
    }
}*/

void in_received_handler(DictionaryIterator *received, void *context) {
    /*
       Called when a message is received from phone. This is the main event driver of the app.
       */
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Incoming message from phone.");

    app_connected = true;
    
    // The INIT key is a response to the init request. This means that the phone is (re)connected.
    if (dict_find(received, INIT_KEY)) {
        if (requestWatchInit) {
            app_timer_cancel(requestWatchInit);
            requestWatchInit = NULL;
        }
        initial = 2;
        app_timer_register(100, requestUpdate, NULL);
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Initialized.");
    }

    // Check if there are any new settings.
    Tuple *largefont_tuple = dict_find(received, LARGEFONT_KEY);
    if (largefont_tuple) {
        setting_largefont = largefont_tuple->value->uint8 != 0;
        setMetarFont();
    }    

    Tuple *battery_tuple = dict_find(received, BAT_KEY);
    if (battery_tuple) {
        setting_bat_save = battery_tuple->value->uint8 != 0;
    }
  
    Tuple *seconds_tuple = dict_find(received, SECONDS_KEY);
    if (seconds_tuple) {
      setting_seconds = seconds_tuple->value->uint8 != 0;
      if (setting_seconds) {
        text_layer_set_font(clock_layer, fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS));
        layer_set_hidden(text_layer_get_layer(date_layer), false);
      } else {
        text_layer_set_font(clock_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_MEDIUM_NUMBERS));
        layer_set_hidden(text_layer_get_layer(date_layer), true);
      } 
    }
 
    // Check if any statuses have changed. A LOCATION_KEY = 1 indicates that the phone has activated the GPS for us.
    // A LOCATION_KEY = 0 indicates that the search has stopped successfully. Do nothing, as we wait for station
    // name.
    // A LOCATION_KEY = -1 indicates that the the search has failed. Request update. If we have a station name
    // from before, this will succeed.
    Tuple *location_tuple = dict_find(received, LOCATION_KEY);
    if (location_tuple) {
        int gps_value = location_tuple->value->uint8;
        if (gps_value == 1) {
            showLayer((Layer *) gps_icon_layer);
        } else {
            hideLayerDelayed((Layer *) gps_icon_layer, 5000);
//            if (gps_value == -1) {
//                app_timer_register(100, requestUpdate, NULL);
//            }
        }
    }

    Tuple *net_tuple = dict_find(received, NET_KEY);
    if (net_tuple) {
        int net_value = net_tuple->value->uint8;
        if (net_value == 1) {
            showLayer((Layer *) net_icon_layer);
        } else {
            hideLayerDelayed((Layer *) net_icon_layer, 5000);
        }
    }

    // Check if we have received a weather update.
  
    // Is a new issued time sent?
    Tuple *updated_tuple = dict_find(received, UPDATED_KEY);
    if (updated_tuple) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Metar was issued %d seconds ago.", (int) (time(NULL) - updated_tuple->value->uint32));
      //metar_update_time = time(NULL) - updated_tuple->value->uint32;
      metar_update_time = updated_tuple->value->uint32;
    }
  
    bool imc_before = imc;
    bool metar_changed = false;

    Tuple *metar_tuple = dict_find(received, METAR_KEY);
    if (metar_tuple) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Metar recieved: %s", metar_tuple->value->cstring);
        if (requestWatchMetar) {
            app_timer_cancel(requestWatchMetar);
            requestWatchMetar = NULL;
        }

        imc = false;

        // Check if we have changed.
        if ((!metar) || (strncmp(metar_tuple->value->cstring, metar, 12) != 0)) {
            metar_changed = true;

            free(metar);
            metar = malloc(strlen(metar_tuple->value->cstring) + 1);
            metar = strcpy(metar, metar_tuple->value->cstring);

            last_weather_update = time(NULL);
            text_layer_set_text(weather_layer, metar);
            resetScrolling();
            setMetarFont();
            
            //light_enable_interaction();
            if (initial > 0) {
                initial--;
            }
            // APP_LOG(APP_LOG_LEVEL_DEBUG, "Metar is different from old, initial is now %d", initial);
 
        } else {
            // APP_LOG(APP_LOG_LEVEL_DEBUG, "Metar unchanged");
        }
    }
  
    // Check if we have received clouds, which is an imc alert.
    Tuple *clouds_tuple = dict_find(received, CLOUDS_KEY);
    if (clouds_tuple) {
        free(dialog_message);
        dialog_message = malloc(strlen(clouds_tuple->value->cstring) + 1);
        dialog_message = strcpy(dialog_message, clouds_tuple->value->cstring);
        dialog_title = "IMC Alert";
        if (metar_changed) {
            showLayer(dialog_layer);
            hideLayerDelayed(dialog_layer, 1 * MINUTES);
        }

        if (!imc_before) {
            vibes_short_pulse();
        }
        imc = true;
    }

    Tuple *station_tuple = dict_find(received, STATION_KEY);
    if (station_tuple) {
        if (requestWatchLocation) {
            app_timer_cancel(requestWatchLocation);
            requestWatchLocation = NULL;
        }
        if ((!station) || (strncmp(station_tuple->value->cstring, station, 12) != 0)) {
            if (station) {
                APP_LOG(APP_LOG_LEVEL_DEBUG, "Freeing station memory.");
                free(station);
            }
            APP_LOG(APP_LOG_LEVEL_DEBUG, "Allocating %d memory for station.", strlen(station_tuple->value->cstring));
            station = malloc(strlen(station_tuple->value->cstring) + 1);
            APP_LOG(APP_LOG_LEVEL_DEBUG, "Setting station to %s.", station);
            station = strcpy(station, station_tuple->value->cstring);

            APP_LOG(APP_LOG_LEVEL_DEBUG, "Station set to: %s", station);
            initial = 2;
        }
        app_timer_register(100, requestUpdate, NULL);
    }

    showStatus();
}

void in_dropped_handler(AppMessageResult reason, void *context) {
    /*
       Called when an incoming message was dropped, e.g. when it's to large for the watch to handle or when the 
       watch is otherwise busy.
       */
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Incoming message dropped!");
}

// }}}

// }}}

//Initialization of app and graphics {{{

static void window_load(Window *window) {
    /*
       Callback for when the main window is loaded.
       */
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    clock_layer = text_layer_create((GRect) { .origin = { 0, 15 }, .size = { bounds.size.w, 65 } });
    text_layer_set_text(clock_layer, "");
    //text_layer_set_font(clock_layer, fonts_get_system_font(FONT_KEY_ROBOTO_BOLD_SUBSET_49));
    text_layer_set_font(clock_layer, fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS));
    text_layer_set_text_alignment(clock_layer, GTextAlignmentCenter);
    text_layer_set_text_color(clock_layer, GColorWhite);
    text_layer_set_background_color(clock_layer, GColorBlack);
    layer_add_child(window_layer, text_layer_get_layer(clock_layer));
  
    date_layer = text_layer_create((GRect) { .origin = { 0, 50 }, .size = { bounds.size.w, 15 } });
    text_layer_set_text(date_layer, "");
    //text_layer_set_font(date_layer, fonts_get_system_font(FONT_KEY_ROBOTO_BOLD_SUBSET_49));
    text_layer_set_font(date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text_alignment(date_layer, GTextAlignmentCenter);
    text_layer_set_text_color(date_layer, GColorWhite);
    text_layer_set_background_color(date_layer, GColorBlack);
    layer_add_child(window_layer, text_layer_get_layer(date_layer));
  
    weather_layer_frame=layer_create((GRect){.origin={0,82},.size={bounds.size.w,82}});
    layer_set_clips(weather_layer_frame, true);

    weather_layer = text_layer_create((GRect) { .origin = { 0, -4 }, .size = { bounds.size.w, 230 } });
    text_layer_set_font(weather_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text(weather_layer, metar);
    text_layer_set_text_alignment(weather_layer, GTextAlignmentCenter);
    text_layer_set_text_color(weather_layer, GColorWhite);
    text_layer_set_background_color(weather_layer, GColorClear);
    layer_add_child(weather_layer_frame, text_layer_get_layer(weather_layer));

    layer_add_child(window_layer, weather_layer_frame);
  
    metar_age_layer = text_layer_create((GRect) { .origin = { 0, 153 }, .size = { bounds.size.w, 15 } });
    text_layer_set_text(metar_age_layer, "Issued 5 minutes ago");
    //text_layer_set_font(metar_age_layer, fonts_get_system_font(FONT_KEY_ROBOTO_BOLD_SUBSET_49));
    text_layer_set_font(metar_age_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text_alignment(metar_age_layer, GTextAlignmentCenter);
    text_layer_set_text_color(metar_age_layer, GColorWhite);
    text_layer_set_background_color(metar_age_layer, GColorBlack);
    layer_add_child(window_layer, text_layer_get_layer(metar_age_layer));



    setMetarFont();

    status_layer = layer_create((GRect) { .origin = { 0, 2 }, .size = { bounds.size.w, 10 } }
);

    imc_icon = gbitmap_create_with_resource(RESOURCE_ID_ICON_IMC);
    GRect imc_bounds = gbitmap_get_bounds(imc_icon);

    imc_icon_layer = bitmap_layer_create((GRect) { .origin = { 15 , 0 }, .size = imc_bounds.size } );    
    bitmap_layer_set_bitmap(imc_icon_layer, imc_icon);
    bitmap_layer_set_alignment(imc_icon_layer, GAlignCenter);
    layer_add_child(status_layer, bitmap_layer_get_layer(imc_icon_layer));
  
    gps_icon = gbitmap_create_with_resource(RESOURCE_ID_ICON_GPS);
    GRect gps_bounds = gbitmap_get_bounds(gps_icon);

    gps_icon_layer = bitmap_layer_create((GRect) { .origin = { bounds.size.w - 36 , 0 }, .size = gps_bounds.size } );    
    bitmap_layer_set_bitmap(gps_icon_layer, gps_icon);
    bitmap_layer_set_alignment(gps_icon_layer, GAlignCenter);
    layer_set_hidden((Layer *) gps_icon_layer, true);
    layer_add_child(status_layer, bitmap_layer_get_layer(gps_icon_layer));

    net_icon = gbitmap_create_with_resource(RESOURCE_ID_ICON_NET);
    GRect net_bounds = gbitmap_get_bounds(net_icon);

    net_icon_layer = bitmap_layer_create((GRect) { .origin = { bounds.size.w - 49, 0 }, .size = net_bounds.size } );    
    bitmap_layer_set_bitmap(net_icon_layer, net_icon);
    bitmap_layer_set_alignment(net_icon_layer, GAlignCenter);
    layer_set_hidden((Layer *) net_icon_layer, true);
    layer_add_child(status_layer, bitmap_layer_get_layer(net_icon_layer));

    conn_icon = gbitmap_create_with_resource(RESOURCE_ID_ICON_CONN);
    GRect conn_bounds = gbitmap_get_bounds(conn_icon);

    conn_icon_layer = bitmap_layer_create((GRect) { .origin = { bounds.size.w - 23 , 0 }, .size = conn_bounds.size } );    
    bitmap_layer_set_bitmap(conn_icon_layer, conn_icon);
    bitmap_layer_set_alignment(conn_icon_layer, GAlignCenter);
    layer_add_child(status_layer, bitmap_layer_get_layer(conn_icon_layer));

    bt_icon = gbitmap_create_with_resource(RESOURCE_ID_ICON_BT);
    GRect bt_bounds = gbitmap_get_bounds(bt_icon);

    bt_icon_layer = bitmap_layer_create((GRect) { .origin = { bounds.size.w - 10, 0 }, .size = bt_bounds.size } );    
    bitmap_layer_set_bitmap(bt_icon_layer, bt_icon);
    bitmap_layer_set_alignment(bt_icon_layer, GAlignCenter);
    layer_add_child(status_layer, bitmap_layer_get_layer(bt_icon_layer));

    layer_add_child(window_layer, status_layer);

    dialog_layer = layer_create((GRect) { .origin = { 10, 78 }, .size = { bounds.size.w-20, 80 } });
    layer_set_hidden(dialog_layer, true);
    layer_set_update_proc(dialog_layer, update_dialog_layer_callback);
    layer_add_child(window_layer, dialog_layer);

    if (persist_exists(STATION_KEY)) {
        station = malloc(10);
        persist_read_string(STATION_KEY, station, 10);
    } else {
        station = NULL;
    }

    time_t now = time(NULL);
    struct tm *current_time = localtime(&now);
    handle_minute_tick(current_time, MINUTE_UNIT);
    showStatus();
    initConnection();
}

static void window_unload(Window *window) {
    /*
       Called when the main window is unloaded.
       Saves persistent data, such as the last Metar and the last station used. Destroys all dynamically allocated
       memory.
     */

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Storing metar '%s'.", metar);
    persist_write_string(METAR_KEY, metar);
    persist_write_string(STATION_KEY, station);

    text_layer_destroy(weather_layer);
    text_layer_destroy(clock_layer);
    text_layer_destroy(date_layer);

    bitmap_layer_destroy(bt_icon_layer);
    bitmap_layer_destroy(gps_icon_layer);
    bitmap_layer_destroy(net_icon_layer);
    bitmap_layer_destroy(conn_icon_layer);
    bitmap_layer_destroy(imc_icon_layer);

    gbitmap_destroy(bt_icon);
    gbitmap_destroy(gps_icon);
    gbitmap_destroy(net_icon);
    gbitmap_destroy(conn_icon);
    gbitmap_destroy(imc_icon);

    layer_destroy(status_layer);
    layer_destroy(dialog_layer);
    layer_destroy(weather_layer_frame);

    property_animation_destroy(weather_animation);

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Freeing Metar.");
    if (metar) 
        free(metar);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Freeing station.");
    if (station)
        free(station);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Freeing dialog.");
    if (dialog_message) {
      free(dialog_message);
    }
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "Freeing dialog title.");
    //free(dialog_title);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Freeing layer timers.");
    free(layer_timers);
}
// }}}

//Initialization of data {{{

static void init() {
    /*
       Initializes the application. Sets appropriate handlers for appropriate events, allocates memory and stuff 
       like that.
    */
    window = window_create();
    window_set_background_color(window, GColorBlack);
    window_set_window_handlers(window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload,
    });

    last_weather_check = time(NULL);

    app_message_register_inbox_received(in_received_handler);
    app_message_register_inbox_dropped(in_dropped_handler);
    app_message_register_outbox_sent(out_sent_handler);
    app_message_register_outbox_failed(out_failed_handler);

    tick_timer_service_subscribe(SECOND_UNIT, &handle_minute_tick);

    bluetooth_connection_service_subscribe(bluetooth_connection_changed);
    accel_tap_service_subscribe(&watch_tapped);

    const uint32_t inbound_size = app_message_inbox_size_maximum();
    // APP_LOG(APP_LOG_LEVEL_DEBUG, "Setting inbox to size %d", (int) inbound_size);
    const uint32_t outbound_size = 128;
    app_message_open(inbound_size, outbound_size);

    dialog_message = malloc(50);
    if (persist_exists(METAR_KEY)) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Found stored metar!");
        int metar_length = persist_get_size(METAR_KEY);
        metar = malloc(metar_length);
        persist_read_string(METAR_KEY, metar, metar_length);
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "No stored metar was found.");
        metar = malloc(200);
    }

    layer_timers = malloc(LAYER_TIMERS * sizeof(LayerTimer));

    const bool animated = true;
//#ifdef PBL_PLATFORM_APLITE
//    window_set_fullscreen(window, true);
//#endif
    window_stack_push(window, animated);
}

static void deinit() {
    /*
       Called when application is closing. Destroys the main window and unsubscribes from event.
       */
    window_destroy(window);
  
    accel_tap_service_unsubscribe();
}

int main() {
    /*
       Main loop. Called by the OS on startup.
       */
    init();

    app_event_loop();

    deinit();
}

// }}}
