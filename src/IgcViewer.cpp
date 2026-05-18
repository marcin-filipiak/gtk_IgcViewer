// igc_viewer.cpp - Native IGC Flight Viewer in C++/GTK3 + WebKit + Linked Brushing

#define VERSION "1"

#include <gtk/gtk.h>
#include <cairo.h>
#include <webkit2/webkit2.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <memory>

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct FlightPoint {
    double lat, lon;      // Decimal degrees
    int alt;              // Meters above sea level
    std::string time;     // HHMMSS format
    int seconds;          // Seconds from midnight
};

struct FlightStats {
    double flightDist = 0;      // Total distance in km
    int maxAlt = -9999, minAlt = 9999, gain = 0;  // Altitude stats in meters
    double maxClimb = 0, maxSink = 0, avgThermalClimb = 0, maxSpeed = 0;  // Performance stats
    size_t pointCount = 0;      // Number of GPS points
};

struct FlightData {
    std::vector<FlightPoint> points;   // All flight points
    std::vector<double> distances;     // Cumulative distance in km
    FlightStats stats;                 // Computed statistics
    bool valid = false;                // Whether data is valid
};

struct ChartData {
    std::vector<double> distances, altitudes;  // Data for altitude chart
};

struct ChartScale {
    double minAlt = 0, maxAlt = 0, altRange = 0, maxDist = 0;
    int pad = 40, plotW = 0, plotH = 0;
    bool hasData = false;
};

struct AppData {
    GtkWidget *window, *statsBox, *chartArea, *statusLabel;
    WebKitWebView *webView;
    FlightData flight;
    ChartData chartData;
    ChartScale chartScale;
    size_t highlightedPoint = SIZE_MAX;  // Index of currently highlighted point
};

// ============================================================================
// FORWARD DECLARATIONS & HELPERS
// ============================================================================
static void updateMap(AppData* app);
static void updateStatsUI(AppData* app);
static void highlightPointOnMap(AppData* app, size_t pointIdx);

// Helper for safely executing JavaScript in WebKit
static void evalJS(WebKitWebView* view, const std::string& script) {
    if (!view) return;
    webkit_web_view_evaluate_javascript(view, script.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static double parseCoordinate(const std::string& raw, char hemisphere) {
    if (raw.length() < 7) return 0.0;
    int degrees, minutes;
    double milliMinutes;
    if (hemisphere == 'N' || hemisphere == 'S') {
        degrees = std::stoi(raw.substr(0, 2));
        minutes = std::stoi(raw.substr(2, 2));
        milliMinutes = std::stoi(raw.substr(4, 3));
    } else {
        degrees = std::stoi(raw.substr(0, 3));
        minutes = std::stoi(raw.substr(3, 2));
        milliMinutes = std::stoi(raw.substr(5, 3));
    }
    double decimal = degrees + minutes / 60.0 + milliMinutes / 60000.0;
    return (hemisphere == 'S' || hemisphere == 'W') ? -decimal : decimal;
}

static double haversine(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371000.0;  // Earth radius in meters
    auto toRad = [](double deg) { return deg * M_PI / 180.0; };
    double dLat = toRad(lat2 - lat1), dLon = toRad(lon2 - lon1);
    double a = sin(dLat/2)*sin(dLat/2) + cos(toRad(lat1))*cos(toRad(lat2))*sin(dLon/2)*sin(dLon/2);
    return R * 2 * atan2(sqrt(a), sqrt(1-a));
}

static double median3(double a, double b, double c) {
    if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
    if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
    return c;
}

static std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return "";
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content(size, '\0');
    file.read(content.data(), size);
    return content;
}

static std::string escapeJS(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\'': oss << "\\'"; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c;
        }
    }
    return oss.str();
}

static FlightData parseIGC(const std::string& content) {
    FlightData data;
    std::regex bRecord(R"(^B(\d{6})(\d{7})([NS])(\d{8})([EW])([AV])(\d{5})(\d{5}))");
    std::istringstream stream(content);
    std::string line;
    
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] != 'B') continue;
        std::smatch m;
        if (!std::regex_search(line, m, bRecord)) continue;
        
        FlightPoint pt;
        pt.lat = parseCoordinate(m[2].str(), m[3].str()[0]);
        pt.lon = parseCoordinate(m[4].str(), m[5].str()[0]);
        int altGPS = std::stoi(m[8].str()), altBaro = std::stoi(m[7].str());
        pt.alt = (altGPS > -500) ? altGPS : altBaro;
        pt.time = m[1].str();
        pt.seconds = std::stoi(pt.time.substr(0,2))*3600 + 
                     std::stoi(pt.time.substr(2,2))*60 + 
                     std::stoi(pt.time.substr(4,2));
        data.points.push_back(pt);
    }
    
    if (data.points.size() < 2) return data;
    data.distances.push_back(0.0);
    
    double cumDist = 0;
    std::vector<double> climbs, speeds;
    
    for (size_t i = 1; i < data.points.size(); ++i) {
        const auto& p1 = data.points[i-1], &p2 = data.points[i];
        double seg = haversine(p1.lat, p1.lon, p2.lat, p2.lon);
        cumDist += seg;
        data.distances.push_back(cumDist / 1000.0);
        
        int dt = p2.seconds - p1.seconds; if (dt <= 0) dt = 1;
        int da = p2.alt - p1.alt;
        double vz = static_cast<double>(da) / dt;
        double gs = (seg / dt) * 3.6;
        
        if (p2.alt > data.stats.maxAlt) data.stats.maxAlt = p2.alt;
        if (p2.alt < data.stats.minAlt) data.stats.minAlt = p2.alt;
        climbs.push_back(vz); speeds.push_back(gs);
    }
    
    // Smooth ground speeds with 3-point median filter
    std::vector<double> smooth;
    for (size_t i = 0; i < speeds.size(); ++i) {
        double a = (i>0)?speeds[i-1]:speeds[i], b=speeds[i], c=(i+1<speeds.size())?speeds[i+1]:speeds[i];
        smooth.push_back(median3(a,b,c));
    }
    data.stats.maxSpeed = *std::max_element(smooth.begin(), smooth.end());
    
    // Filter climb rates to remove GPS noise (-10 to +10 m/s)
    std::vector<double> validClimbs;
    for (double v : climbs) if (v > -10 && v < 10) validClimbs.push_back(v);
    if (!validClimbs.empty()) {
        std::vector<double> pos, neg;
        for (double v : validClimbs) (v > 0 ? pos : neg).push_back(v);
        if (!pos.empty()) data.stats.maxClimb = *std::max_element(pos.begin(), pos.end());
        if (!neg.empty()) data.stats.maxSink = *std::min_element(neg.begin(), neg.end());
        
        // Thermal detection: consecutive climbs > 0.3 m/s
        std::vector<double> thermals;
        bool inThermal = false; double thSum = 0; int thCnt = 0;
        for (double v : climbs) {
            if (v > 0.3) { if (!inThermal) { inThermal=true; thSum=0; thCnt=0; } thSum+=v; thCnt++; }
            else if (inThermal) { if (thCnt>=2) thermals.push_back(thSum/thCnt); inThermal=false; }
        }
        if (!thermals.empty()) {
            double sum = std::accumulate(thermals.begin(), thermals.end(), 0.0);
            data.stats.avgThermalClimb = sum / thermals.size();
        }
    }
    
    data.stats.flightDist = cumDist / 1000.0;
    data.stats.gain = std::max(0, data.stats.maxAlt - data.stats.minAlt);
    data.stats.pointCount = data.points.size();
    data.valid = true;
    return data;
}

// ============================================================================
// ALTITUDE CHART (Cairo) + INTERACTION
// ============================================================================

static void on_chart_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    AppData* app = static_cast<AppData*>(user_data);
    ChartData* cd = &app->chartData;
    if (!cd || cd->altitudes.empty()) return;
    
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    int pad = 40, plotW = w - 2*pad, plotH = h - 2*pad;
    
    // Background
    cairo_set_source_rgb(cr, 0.06, 0.09, 0.14);
    cairo_paint(cr);
    
    if (cd->altitudes.size() < 2) return;
    
    double minAlt = *std::min_element(cd->altitudes.begin(), cd->altitudes.end());
    double maxAlt = *std::max_element(cd->altitudes.begin(), cd->altitudes.end());
    double altRange = std::max(1.0, maxAlt - minAlt);
    double maxDist = cd->distances.back();
    
    // Store scale parameters for click interaction
    app->chartScale = {minAlt, maxAlt, altRange, maxDist, pad, plotW, plotH, true};
    
    // Grid lines
    cairo_set_source_rgba(cr, 0.2, 0.25, 0.35, 0.4);
    cairo_set_line_width(cr, 0.5);
    for (int i = 0; i <= 4; ++i) {
        double y = pad + plotH - (i * plotH / 4.0);
        cairo_move_to(cr, pad, y); cairo_rel_line_to(cr, plotW, 0); cairo_stroke(cr);
    }
    
    // Altitude profile line
    cairo_set_source_rgb(cr, 0.54, 0.36, 0.96);  // #8b5cf6
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, pad, pad + plotH - (cd->altitudes[0]-minAlt)/altRange*plotH);
    for (size_t i = 1; i < cd->altitudes.size(); ++i) {
        double x = pad + (cd->distances[i]/maxDist)*plotW;
        double y = pad + plotH - (cd->altitudes[i]-minAlt)/altRange*plotH;
        cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    
    // Highlight selected point
    if (app->highlightedPoint < cd->altitudes.size()) {
        size_t idx = app->highlightedPoint;
        double x = pad + (cd->distances[idx]/maxDist)*plotW;
        double y = pad + plotH - (cd->altitudes[idx]-minAlt)/altRange*plotH;
        
        // Vertical guide line
        cairo_set_source_rgba(cr, 0.96, 0.65, 0.11, 0.5);  // #f59e0b with alpha
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, x, pad); cairo_line_to(cr, x, pad+plotH); cairo_stroke(cr);
        
        // Point marker
        cairo_set_source_rgb(cr, 0.96, 0.65, 0.11);
        cairo_arc(cr, x, y, 5, 0, 2*M_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_arc(cr, x, y, 2.5, 0, 2*M_PI);
        cairo_fill(cr);
    }
    
    // Axis labels
    cairo_set_source_rgb(cr, 0.65, 0.72, 0.82);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    
    // Y-axis (altitude)
    for (int i = 0; i <= 4; ++i) {
        double val = minAlt + (i * altRange / 4.0);
        double y = pad + plotH - (i * plotH / 4.0);
        std::ostringstream oss; oss << static_cast<int>(val) << "m";
        cairo_move_to(cr, 5, y+3); cairo_show_text(cr, oss.str().c_str());
    }
    // X-axis (distance)
    for (int i = 0; i <= 4; ++i) {
        double val = (i * maxDist / 4.0);
        double x = pad + (i * plotW / 4.0);
        std::ostringstream oss; oss << std::fixed << std::setprecision(1) << val << "km";
        cairo_move_to(cr, x-15, h-10); cairo_show_text(cr, oss.str().c_str());
    }
}

static gboolean on_chart_clicked(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
    AppData* app = static_cast<AppData*>(user_data);
    if (!app->chartScale.hasData || event->button != GDK_BUTTON_PRIMARY) 
        return GDK_EVENT_PROPAGATE;
    
    const auto& cs = app->chartScale;
    double clickX = event->x;
    
    // Click outside chart area → clear highlight
    if (clickX < cs.pad || clickX > cs.pad + cs.plotW) {
        evalJS(app->webView, "if(window._highlightMarker) { map.removeLayer(window._highlightMarker); delete window._highlightMarker; }");
        app->highlightedPoint = SIZE_MAX;
        gtk_widget_queue_draw(app->chartArea);
        return GDK_EVENT_STOP;
    }
    
    // Convert X coordinate to distance (km)
    double distKm = ((clickX - cs.pad) / cs.plotW) * cs.maxDist;
    
    // Find nearest point by distance
    size_t bestIdx = 0;
    double bestDiff = std::abs(app->flight.distances[0] - distKm);
    for (size_t i = 1; i < app->flight.distances.size(); ++i) {
        double diff = std::abs(app->flight.distances[i] - distKm);
        if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
    }
    
    app->highlightedPoint = bestIdx;
    gtk_widget_queue_draw(app->chartArea);
    highlightPointOnMap(app, bestIdx);
    
    return GDK_EVENT_STOP;
}

// ============================================================================
// MAP (WebKit + Leaflet)
// ============================================================================

static void highlightPointOnMap(AppData* app, size_t pointIdx) {
    if (!app->webView || pointIdx >= app->flight.points.size()) return;
    
    const auto& pt = app->flight.points[pointIdx];
    double dist = app->flight.distances[pointIdx];
    
    std::ostringstream js;
    js << std::fixed << std::setprecision(6);
    js << "(function() { "
       << "if(window._highlightMarker) map.removeLayer(window._highlightMarker); "
       << "const popup = L.popup().setContent('"
       << "<b style=\\'color:#fbbf24\\'>Point #" << (pointIdx+1) << "</b><br>"
       << "Time: " << escapeJS(pt.time) << "<br>"
       << "Altitude: " << pt.alt << " m<br>"
       << "Distance: " << std::setprecision(2) << dist << " km"
       << "'); "
       << "window._highlightMarker = L.circleMarker([" 
       << pt.lat << "," << pt.lon << "], { "
       << "radius: 9, color: '#f59e0b', fillColor: '#fbbf24', fillOpacity: 0.95, weight: 2.5 "
       << "}).addTo(map).bindPopup(popup).openPopup(); "
       << "})();";
    
    evalJS(app->webView, js.str());
}

static void updateMap(AppData* app) {
    if (!app->webView || app->flight.points.empty()) return;
    
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < app->flight.points.size(); ++i) {
        json << "[" << std::fixed << std::setprecision(6) 
             << app->flight.points[i].lat << ","
             << app->flight.points[i].lon << "]";
        if (i + 1 < app->flight.points.size()) json << ",";
    }
    json << "]";
    
    std::string html = R"html(
<!DOCTYPE html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<style>
html,body,#map{margin:0;padding:0;width:100%;height:100%;background:#0a0e17}
.leaflet-control-zoom a{background:#1e293b!important;color:#f1f5f9!important;border-color:#334155!important}
.leaflet-control-zoom a:hover{background:#38bdf8!important;color:#0a0e17!important}
.leaflet-popup-content-wrapper,.leaflet-popup-tip{background:#0f172a!important;color:#f1f5f9!important;border:1px solid #334155!important}
</style></head><body><div id="map"></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const pts = )html" + json.str() + R"html(;
const map = L.map('map',{zoomControl:true,attributionControl:false,zoomAnimation:true}).setView([pts[0][0],pts[0][1]],10);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap'}).addTo(map);
L.polyline(pts,{color:'#38bdf8',weight:3,opacity:0.9,lineCap:'round',lineJoin:'round'}).addTo(map);
const mk = (c)=>L.divIcon({html:`<div style="width:12px;height:12px;border-radius:50%;background:${c};border:2px solid #fff;box-shadow:0 2px 8px rgba(0,0,0,0.4)"></div>`,iconSize:[12,12],className:''});
L.marker([pts[0][0],pts[0][1]],{icon:mk('#10b981')}).addTo(map).bindPopup('Start');
L.marker([pts[pts.length-1][0],pts[pts.length-1][1]],{icon:mk('#ef4444')}).addTo(map).bindPopup('End');
setTimeout(()=>map.fitBounds(L.latLngBounds(pts),{padding:[35,35],maxZoom:14}),300);
</script></body></html>)html";
    
    webkit_web_view_load_html(app->webView, html.c_str(), nullptr);
}

// ============================================================================
// STATISTICS UI
// ============================================================================

static void updateStatsUI(AppData* app) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(app->statsBox));
    for (GList* l = children; l != nullptr; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    
    auto addCard = [&](const char* title, const char* value, const char* unit, const char* note=nullptr) {
        GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(card, 8); gtk_widget_set_margin_end(card, 8);
        gtk_widget_set_margin_top(card, 8); gtk_widget_set_margin_bottom(card, 8);
        gtk_widget_set_tooltip_text(card, title);
        
        GtkWidget* lblTitle = gtk_label_new(title);
        gtk_label_set_use_markup(GTK_LABEL(lblTitle), TRUE);
        gtk_widget_set_halign(lblTitle, GTK_ALIGN_START);
        gtk_widget_set_opacity(lblTitle, 0.7);
        
        std::string valStr = std::string(value) + " <span size='small' alpha='70%'>" + unit + "</span>";
        GtkWidget* lblValue = gtk_label_new(valStr.c_str());
        gtk_label_set_use_markup(GTK_LABEL(lblValue), TRUE);
        gtk_widget_set_halign(lblValue, GTK_ALIGN_START);
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_attr_list_insert(attrs, pango_attr_scale_new(1.4));
        gtk_label_set_attributes(GTK_LABEL(lblValue), attrs);
        pango_attr_list_unref(attrs);
        
        gtk_box_pack_start(GTK_BOX(card), lblTitle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), lblValue, FALSE, FALSE, 0);
        if (note) {
            GtkWidget* lblNote = gtk_label_new(note);
            gtk_widget_set_halign(lblNote, GTK_ALIGN_START);
            gtk_widget_set_opacity(lblNote, 0.6);
            gtk_box_pack_start(GTK_BOX(card), lblNote, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(app->statsBox), card, TRUE, TRUE, 0);
    };
    
    auto fmt = [](double v, int prec=1) {
        std::ostringstream oss; oss << std::fixed << std::setprecision(prec) << v; return oss.str();
    };
    
    addCard("Flight Distance", fmt(app->flight.stats.flightDist).c_str(), "km", "including thermal circling");
    addCard("Max Altitude", std::to_string(app->flight.stats.maxAlt).c_str(), "m");
    addCard("Altitude Gain", std::to_string(app->flight.stats.gain).c_str(), "m");
    addCard("Max Speed", fmt(app->flight.stats.maxSpeed).c_str(), "km/h", "GPS • estimated");
    addCard("Max Climb", fmt(app->flight.stats.maxClimb).c_str(), "m/s");
    addCard("Avg Thermal", fmt(app->flight.stats.avgThermalClimb).c_str(), "m/s");
    
    gtk_widget_show_all(app->statsBox);
}

// ============================================================================
// GTK CALLBACKS
// ============================================================================

static void on_file_selected(GtkFileChooser* chooser, gpointer user_data) {
    AppData* app = static_cast<AppData*>(user_data);
    char* path = gtk_file_chooser_get_filename(chooser);
    if (!path) return;
    
    std::string content = readFile(path);
    g_free(path);
    
    if (content.empty()) {
        gtk_label_set_text(GTK_LABEL(app->statusLabel), "⚠️ Could not read file.");
        return;
    }
    
    app->flight = parseIGC(content);
    if (!app->flight.valid) {
        gtk_label_set_text(GTK_LABEL(app->statusLabel), "⚠️ File does not contain enough GPS records.");
        return;
    }
    
    app->chartData.distances = app->flight.distances;
    app->chartData.altitudes.clear();
    for (const auto& p : app->flight.points) app->chartData.altitudes.push_back(p.alt);
    
    std::string status = "✓ Loaded " + std::to_string(app->flight.stats.pointCount) + " GPS points";
    gtk_label_set_text(GTK_LABEL(app->statusLabel), status.c_str());
    gtk_widget_set_opacity(app->statusLabel, 1.0);
    
    app->highlightedPoint = SIZE_MAX;
    updateStatsUI(app);
    updateMap(app);
    gtk_widget_queue_draw(app->chartArea);
}

static void on_load_clicked(GtkWidget*, gpointer user_data) {
    AppData* app = static_cast<AppData*>(user_data);
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Select IGC File",
        GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "IGC Files (*.igc)");
    gtk_file_filter_add_pattern(filter, "*.igc");
    gtk_file_filter_add_pattern(filter, "*.IGC");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        on_file_selected(GTK_FILE_CHOOSER(dialog), app);
    gtk_widget_destroy(dialog);
}

static gboolean on_window_close(GtkWidget*, GdkEvent*, gpointer) {
    gtk_main_quit();
    return FALSE;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    
    AppData app{};
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "✈️ IGC Flight Viewer v" VERSION);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 950, 800);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 12);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(on_window_close), nullptr);
    
    GtkWidget* mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    
    // Header
    GtkWidget* header = gtk_label_new(nullptr);
    std::string headerTxt = "<span size='x-large' weight='bold'>✈️ IGC Flight Viewer</span>\n"
                           "<span size='small' alpha='70%'>Flight analysis</span>";
    gtk_label_set_markup(GTK_LABEL(header), headerTxt.c_str());
    gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(mainBox), header, FALSE, FALSE, 4);
    
    // Top bar
    GtkWidget* topBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* loadBtn = gtk_button_new_with_label("📁 Load IGC File");
    gtk_widget_set_halign(loadBtn, GTK_ALIGN_START);
    g_signal_connect(loadBtn, "clicked", G_CALLBACK(on_load_clicked), &app);
    
    app.statusLabel = gtk_label_new("Select an .igc file to start analysis.");
    gtk_widget_set_halign(app.statusLabel, GTK_ALIGN_START);
    gtk_widget_set_opacity(app.statusLabel, 0.8);
    
    gtk_box_pack_start(GTK_BOX(topBar), loadBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(topBar), app.statusLabel, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(mainBox), topBar, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(mainBox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    
    // Statistics cards
    GtkWidget* statsFrame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(statsFrame), GTK_SHADOW_NONE);
    app.statsBox = gtk_flow_box_new();
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(app.statsBox), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(app.statsBox), 10);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(app.statsBox), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(app.statsBox), 3);
    gtk_container_add(GTK_CONTAINER(statsFrame), app.statsBox);
    gtk_box_pack_start(GTK_BOX(mainBox), statsFrame, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(mainBox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    
    // Map frame
    GtkWidget* mapFrame = gtk_frame_new("🗺️ Flight Track (click chart to highlight a point)");
    gtk_frame_set_shadow_type(GTK_FRAME(mapFrame), GTK_SHADOW_NONE);
    app.webView = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_widget_set_size_request(GTK_WIDGET(app.webView), -1, 300);
    gtk_container_add(GTK_CONTAINER(mapFrame), GTK_WIDGET(app.webView));
    gtk_box_pack_start(GTK_BOX(mainBox), mapFrame, TRUE, TRUE, 0);
    
    // Altitude chart frame
    GtkWidget* chartFrame = gtk_frame_new("📈 Altitude Profile (click to highlight point on map)");
    gtk_frame_set_shadow_type(GTK_FRAME(chartFrame), GTK_SHADOW_NONE);
    app.chartArea = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.chartArea, -1, 220);
    gtk_widget_set_app_paintable(app.chartArea, TRUE);
    gtk_widget_add_events(app.chartArea, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(app.chartArea, "draw", G_CALLBACK(on_chart_draw), &app);
    g_signal_connect(app.chartArea, "button-press-event", G_CALLBACK(on_chart_clicked), &app);
    
    gtk_container_add(GTK_CONTAINER(chartFrame), app.chartArea);
    gtk_box_pack_start(GTK_BOX(mainBox), chartFrame, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(app.window), mainBox);
    gtk_widget_show_all(app.window);
    
    gtk_main();
    return 0;
}
