#include <gtk/gtk.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr char kApplicationId[] = "com.skot.AmScope";
constexpr char kCameraName[] = "DM756-U830";

struct Resolution {
  int width;
  int height;

  bool operator==(const Resolution&) const = default;
};

struct CameraInfo {
  std::string device;
  std::vector<Resolution> resolutions;
};

int resolution_priority(const Resolution& resolution) {
  static constexpr Resolution preferred[] = {
      {1920, 1080}, {3840, 2160}, {1280, 720}, {960, 540}};

  for (size_t i = 0; i < std::size(preferred); ++i) {
    if (resolution == preferred[i]) {
      return static_cast<int>(i);
    }
  }
  return 100;
}

std::optional<CameraInfo> find_camera() {
  for (int index = 0; index < 64; ++index) {
    const std::string device = "/dev/video" + std::to_string(index);
    const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }

    v4l2_capability capabilities{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capabilities) < 0) {
      close(fd);
      continue;
    }

    const std::string card(reinterpret_cast<char*>(capabilities.card));
    const uint32_t device_capabilities =
        capabilities.capabilities & V4L2_CAP_DEVICE_CAPS
            ? capabilities.device_caps
            : capabilities.capabilities;

    if (card.find(kCameraName) == std::string::npos ||
        !(device_capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
      close(fd);
      continue;
    }

    std::vector<Resolution> resolutions;
    v4l2_fmtdesc format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (format.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0;
         ++format.index) {
      if (format.pixelformat != V4L2_PIX_FMT_MJPEG &&
          format.pixelformat != V4L2_PIX_FMT_JPEG) {
        continue;
      }

      v4l2_frmsizeenum frame_size{};
      frame_size.pixel_format = format.pixelformat;
      for (frame_size.index = 0;
           ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) == 0;
           ++frame_size.index) {
        if (frame_size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
          continue;
        }
        Resolution resolution{static_cast<int>(frame_size.discrete.width),
                              static_cast<int>(frame_size.discrete.height)};
        if (std::find(resolutions.begin(), resolutions.end(), resolution) ==
            resolutions.end()) {
          resolutions.push_back(resolution);
        }
      }
    }
    close(fd);

    if (resolutions.empty()) {
      continue;
    }

    std::stable_sort(resolutions.begin(), resolutions.end(),
                     [](const Resolution& left, const Resolution& right) {
                       const int left_priority = resolution_priority(left);
                       const int right_priority = resolution_priority(right);
                       if (left_priority != right_priority) {
                         return left_priority < right_priority;
                       }
                       return left.width * left.height > right.width * right.height;
                     });

    return CameraInfo{device, std::move(resolutions)};
  }

  return std::nullopt;
}

std::string resolution_text(const Resolution& resolution) {
  return std::to_string(resolution.width) + "×" +
         std::to_string(resolution.height);
}

void apply_orientation(const cv::Mat& source, cv::Mat& destination,
                       int rotation_quarters, bool mirrored) {
  cv::Mat rotated;
  switch (rotation_quarters % 4) {
    case 1:
      cv::rotate(source, rotated, cv::ROTATE_90_CLOCKWISE);
      break;
    case 2:
      cv::rotate(source, rotated, cv::ROTATE_180);
      break;
    case 3:
      cv::rotate(source, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
      break;
    default:
      rotated = source;
      break;
  }

  if (mirrored) {
    cv::flip(rotated, destination, 1);
  } else {
    destination = rotated;
  }
}

std::string capture_directory() {
  const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
  const std::filesystem::path base =
      pictures && *pictures ? pictures : std::filesystem::path(g_get_home_dir()) / "Pictures";
  return (base / "AmScope Captures").string();
}

class MicroscopeApp {
 public:
  explicit MicroscopeApp(GtkApplication* application,
                         std::optional<Resolution> initial_resolution)
      : application_(application), requested_resolution_(initial_resolution.value_or(Resolution{1920, 1080})) {}

  ~MicroscopeApp() { stop_worker(); }

  void activate() {
    if (window_) {
      gtk_window_present(GTK_WINDOW(window_));
      return;
    }

    build_ui();
    gtk_widget_show_all(window_);
    start_worker();
  }

  void stop_worker() {
    stopping_ = true;
    restart_requested_ = true;
    if (worker_.joinable()) {
      worker_.join();
    }
    if (preview_pixbuf_) {
      g_object_unref(preview_pixbuf_);
      preview_pixbuf_ = nullptr;
    }
  }

 private:
  struct StatusUpdate {
    MicroscopeApp* app;
    std::string message;
    bool connected;
  };

  struct ModesUpdate {
    MicroscopeApp* app;
    std::string device;
    std::vector<Resolution> resolutions;
  };

  GtkApplication* application_ = nullptr;
  GtkWidget* window_ = nullptr;
  GtkWidget* drawing_area_ = nullptr;
  GtkWidget* status_label_ = nullptr;
  GtkWidget* status_dot_ = nullptr;
  GtkWidget* resolution_combo_ = nullptr;
  GtkWidget* capture_button_ = nullptr;
  GtkWidget* open_button_ = nullptr;
  GtkWidget* mirror_button_ = nullptr;

  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> restart_requested_{false};
  std::atomic<bool> preview_update_pending_{false};
  std::atomic<int> rotation_quarters_{0};
  std::atomic<bool> mirrored_{false};

  std::mutex config_mutex_;
  Resolution requested_resolution_;

  std::mutex frame_mutex_;
  cv::Mat latest_frame_;

  std::mutex preview_mutex_;
  cv::Mat latest_preview_rgb_;
  GdkPixbuf* preview_pixbuf_ = nullptr;

  bool changing_combo_ = false;
  bool camera_connected_ui_ = false;
  std::string current_device_;
  std::vector<Resolution> current_resolutions_;

  void build_ui() {
    window_ = gtk_application_window_new(application_);
    gtk_window_set_title(GTK_WINDOW(window_), "AmScope Microscope");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 820);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "camera-web");

    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "AmScope Microscope");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), kCameraName);
    gtk_window_set_titlebar(GTK_WINDOW(window_), header);

    mirror_button_ = gtk_toggle_button_new_with_label("Mirror");
    gtk_widget_set_tooltip_text(mirror_button_, "Mirror the displayed image horizontally (M)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), mirror_button_);
    g_signal_connect(mirror_button_, "toggled", G_CALLBACK(on_mirror_toggled), this);

    GtkWidget* rotate_button = gtk_button_new_with_label("Rotate 90°");
    gtk_widget_set_tooltip_text(rotate_button, "Rotate the displayed image clockwise (R)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), rotate_button);
    g_signal_connect(rotate_button, "clicked", G_CALLBACK(on_rotate_clicked), this);

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), main_box);

    drawing_area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(drawing_area_, TRUE);
    gtk_widget_set_vexpand(drawing_area_, TRUE);
    gtk_widget_set_size_request(drawing_area_, 640, 360);
    gtk_box_pack_start(GTK_BOX(main_box), drawing_area_, TRUE, TRUE, 0);
    g_signal_connect(drawing_area_, "draw", G_CALLBACK(on_draw), this);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(toolbar, 14);
    gtk_widget_set_margin_end(toolbar, 14);
    gtk_widget_set_margin_top(toolbar, 12);
    gtk_widget_set_margin_bottom(toolbar, 12);
    gtk_box_pack_start(GTK_BOX(main_box), toolbar, FALSE, FALSE, 0);

    GtkWidget* resolution_label = gtk_label_new("Resolution");
    gtk_box_pack_start(GTK_BOX(toolbar), resolution_label, FALSE, FALSE, 0);

    resolution_combo_ = gtk_combo_box_text_new();
    gtk_widget_set_sensitive(resolution_combo_, FALSE);
    gtk_box_pack_start(GTK_BOX(toolbar), resolution_combo_, FALSE, FALSE, 0);
    g_signal_connect(resolution_combo_, "changed", G_CALLBACK(on_resolution_changed), this);

    capture_button_ = gtk_button_new_with_label("Capture JPG");
    GtkStyleContext* capture_style = gtk_widget_get_style_context(capture_button_);
    gtk_style_context_add_class(capture_style, "suggested-action");
    gtk_widget_set_sensitive(capture_button_, FALSE);
    gtk_widget_set_tooltip_text(capture_button_, "Save the current full-resolution frame (Space or Ctrl+S)");
    gtk_box_pack_start(GTK_BOX(toolbar), capture_button_, FALSE, FALSE, 0);
    g_signal_connect(capture_button_, "clicked", G_CALLBACK(on_capture_clicked), this);

    open_button_ = gtk_button_new_with_label("Open Captures");
    gtk_box_pack_start(GTK_BOX(toolbar), open_button_, FALSE, FALSE, 0);
    g_signal_connect(open_button_, "clicked", G_CALLBACK(on_open_clicked), this);

    GtkWidget* reconnect_button = gtk_button_new_with_label("Reconnect");
    gtk_widget_set_tooltip_text(reconnect_button, "Rescan USB video devices");
    gtk_box_pack_end(GTK_BOX(toolbar), reconnect_button, FALSE, FALSE, 0);
    g_signal_connect(reconnect_button, "clicked", G_CALLBACK(on_reconnect_clicked), this);

    GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_margin_start(status_box, 14);
    gtk_widget_set_margin_end(status_box, 14);
    gtk_widget_set_margin_bottom(status_box, 10);
    gtk_box_pack_start(GTK_BOX(main_box), status_box, FALSE, FALSE, 0);

    status_dot_ = gtk_label_new("●");
    gtk_box_pack_start(GTK_BOX(status_box), status_dot_, FALSE, FALSE, 0);
    status_label_ = gtk_label_new("Scanning USB devices…");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0F);
    gtk_widget_set_hexpand(status_label_, TRUE);
    gtk_box_pack_start(GTK_BOX(status_box), status_label_, TRUE, TRUE, 0);

    GtkWidget* shortcut_label = gtk_label_new("Space / Ctrl+S to capture");
    GtkStyleContext* shortcut_style = gtk_widget_get_style_context(shortcut_label);
    gtk_style_context_add_class(shortcut_style, "dim-label");
    gtk_box_pack_end(GTK_BOX(status_box), shortcut_label, FALSE, FALSE, 0);

    g_signal_connect(window_, "key-press-event", G_CALLBACK(on_key_press), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(on_window_destroy), this);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        css,
        "drawingarea { background: #000; }"
        ".camera-connected { color: #57e389; }"
        ".camera-disconnected { color: #f66151; }",
        -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    set_status_ui("Scanning USB devices…", false);
  }

  void start_worker() {
    stopping_ = false;
    worker_ = std::thread([this] { camera_loop(); });
  }

  void camera_loop() {
    while (!stopping_) {
      auto camera = find_camera();
      if (!camera) {
        post_status("Waiting for AmScope DM756-U830…", false);
        interruptible_wait(std::chrono::seconds(2));
        continue;
      }

      post_modes(camera->device, camera->resolutions);

      Resolution resolution;
      {
        std::lock_guard lock(config_mutex_);
        resolution = requested_resolution_;
      }
      if (std::find(camera->resolutions.begin(), camera->resolutions.end(), resolution) ==
          camera->resolutions.end()) {
        resolution = camera->resolutions.front();
        std::lock_guard lock(config_mutex_);
        requested_resolution_ = resolution;
      }

      restart_requested_ = false;
      cv::VideoCapture capture;
      const std::vector<int> capture_parameters = {
          cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
          cv::CAP_PROP_FRAME_WIDTH, resolution.width,
          cv::CAP_PROP_FRAME_HEIGHT, resolution.height,
          cv::CAP_PROP_FPS, 30};
      if (!capture.open(camera->device, cv::CAP_V4L2, capture_parameters)) {
        post_status("Camera found, but could not open it. Close other camera apps and retry.", false);
        interruptible_wait(std::chrono::seconds(2));
        continue;
      }

      capture.set(cv::CAP_PROP_BUFFERSIZE, 2);

      const int actual_width = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH));
      const int actual_height = static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT));
      if (actual_width > 0 && actual_height > 0) {
        resolution = {actual_width, actual_height};
      }

      post_status("Connected on " + camera->device + " at " + resolution_text(resolution), true);

      int failed_reads = 0;
      auto next_preview_at = std::chrono::steady_clock::now();
      while (!stopping_ && !restart_requested_) {
        cv::Mat frame;
        if (!capture.read(frame) || frame.empty()) {
          if (++failed_reads >= 15) {
            break;
          }
          continue;
        }
        failed_reads = 0;

        {
          std::lock_guard lock(frame_mutex_);
          latest_frame_ = frame;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next_preview_at) {
          continue;
        }
        next_preview_at = now + std::chrono::milliseconds(66);

        cv::Mat preview;
        constexpr double kMaximumPreviewWidth = 1280.0;
        constexpr double kMaximumPreviewHeight = 720.0;
        const double scale = std::min(
            {1.0, kMaximumPreviewWidth / frame.cols, kMaximumPreviewHeight / frame.rows});
        if (scale < 1.0) {
          cv::resize(frame, preview, cv::Size(), scale, scale, cv::INTER_AREA);
        } else {
          preview = frame;
        }

        cv::Mat preview_rgb;
        cv::cvtColor(preview, preview_rgb, cv::COLOR_BGR2RGB);
        {
          std::lock_guard lock(preview_mutex_);
          latest_preview_rgb_ = std::move(preview_rgb);
        }
        schedule_preview_update();
      }

      capture.release();
      {
        std::lock_guard lock(frame_mutex_);
        latest_frame_.release();
      }

      if (!stopping_ && !restart_requested_) {
        post_status("Camera disconnected. Scanning for it…", false);
        interruptible_wait(std::chrono::seconds(1));
      }
    }
  }

  void interruptible_wait(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!stopping_ && !restart_requested_ && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void schedule_preview_update() {
    if (!preview_update_pending_.exchange(true)) {
      g_idle_add(on_preview_ready, this);
    }
  }

  void post_status(std::string message, bool connected) {
    g_idle_add(
        [](gpointer data) -> gboolean {
          auto* update = static_cast<StatusUpdate*>(data);
          if (!update->app->stopping_) {
            update->app->set_status_ui(update->message, update->connected);
          }
          delete update;
          return G_SOURCE_REMOVE;
        },
        new StatusUpdate{this, std::move(message), connected});
  }

  void post_modes(std::string device, std::vector<Resolution> resolutions) {
    g_idle_add(
        [](gpointer data) -> gboolean {
          auto* update = static_cast<ModesUpdate*>(data);
          if (!update->app->stopping_) {
            update->app->set_modes_ui(update->device, update->resolutions);
          }
          delete update;
          return G_SOURCE_REMOVE;
        },
        new ModesUpdate{this, std::move(device), std::move(resolutions)});
  }

  void set_modes_ui(const std::string& device,
                    const std::vector<Resolution>& resolutions) {
    if (device == current_device_ && resolutions == current_resolutions_) {
      return;
    }
    current_device_ = device;
    current_resolutions_ = resolutions;

    Resolution selected;
    {
      std::lock_guard lock(config_mutex_);
      selected = requested_resolution_;
    }

    changing_combo_ = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(resolution_combo_));
    int active_index = 0;
    for (size_t index = 0; index < resolutions.size(); ++index) {
      const std::string text = resolution_text(resolutions[index]);
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(resolution_combo_), text.c_str());
      if (resolutions[index] == selected) {
        active_index = static_cast<int>(index);
      }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(resolution_combo_), active_index);
    gtk_widget_set_sensitive(resolution_combo_, TRUE);
    changing_combo_ = false;
  }

  void set_status_ui(const std::string& message, bool connected) {
    camera_connected_ui_ = connected;
    gtk_label_set_text(GTK_LABEL(status_label_), message.c_str());
    GtkStyleContext* context = gtk_widget_get_style_context(status_dot_);
    gtk_style_context_remove_class(context, "camera-connected");
    gtk_style_context_remove_class(context, "camera-disconnected");
    gtk_style_context_add_class(
        context, connected ? "camera-connected" : "camera-disconnected");
    gtk_widget_set_sensitive(capture_button_, connected);
  }

  void capture_jpg() {
    cv::Mat frame;
    {
      std::lock_guard lock(frame_mutex_);
      if (!latest_frame_.empty()) {
        frame = latest_frame_.clone();
      }
    }

    if (frame.empty()) {
      set_status_ui("No camera frame is available yet.", false);
      return;
    }

    cv::Mat oriented_frame;
    apply_orientation(frame, oriented_frame, rotation_quarters_.load(),
                      mirrored_.load());

    try {
      const std::filesystem::path directory(capture_directory());
      std::filesystem::create_directories(directory);

      const auto now = std::chrono::system_clock::now();
      const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now.time_since_epoch()) %
                                1000;
      const std::time_t time = std::chrono::system_clock::to_time_t(now);
      std::tm local_time{};
      localtime_r(&time, &local_time);

      std::ostringstream filename;
      filename << "DM756_" << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S")
               << '-' << std::setfill('0') << std::setw(3) << milliseconds.count()
               << ".jpg";
      const std::filesystem::path path = directory / filename.str();

      if (!cv::imwrite(path.string(), oriented_frame, {cv::IMWRITE_JPEG_QUALITY, 95})) {
        throw std::runtime_error("JPEG encoder failed");
      }

      set_status_ui("Saved " + path.string(), true);
      GNotification* notification = g_notification_new("Microscope image captured");
      g_notification_set_body(notification, path.string().c_str());
      g_application_send_notification(G_APPLICATION(application_), "capture", notification);
      g_object_unref(notification);
    } catch (const std::exception& error) {
      set_status_ui(std::string("Could not save image: ") + error.what(), false);
    }
  }

  void open_captures() {
    try {
      const std::filesystem::path directory(capture_directory());
      std::filesystem::create_directories(directory);
      gchar* uri = g_filename_to_uri(directory.string().c_str(), nullptr, nullptr);
      if (uri) {
        GError* error = nullptr;
        g_app_info_launch_default_for_uri(uri, nullptr, &error);
        if (error) {
          set_status_ui(std::string("Could not open captures: ") + error->message, false);
          g_error_free(error);
        }
        g_free(uri);
      }
    } catch (const std::exception& error) {
      set_status_ui(std::string("Could not open captures: ") + error.what(), false);
    }
  }

  void rotate_view() {
    const int rotation = (rotation_quarters_.fetch_add(1) + 1) % 4;
    rotation_quarters_ = rotation;
    gtk_widget_queue_draw(drawing_area_);
    set_status_ui("View rotated to " + std::to_string(rotation * 90) + "°" +
                      (mirrored_ ? " and mirrored" : ""),
                  camera_connected_ui_);
  }

  void set_mirrored(bool mirrored) {
    mirrored_ = mirrored;
    gtk_widget_queue_draw(drawing_area_);
    set_status_ui(
        "View rotated to " + std::to_string(rotation_quarters_.load() * 90) + "°" +
            (mirrored ? " and mirrored" : ""),
        camera_connected_ui_);
  }

  static gboolean on_preview_ready(gpointer data) {
    auto* app = static_cast<MicroscopeApp*>(data);
    cv::Mat preview;
    {
      std::lock_guard lock(app->preview_mutex_);
      if (!app->latest_preview_rgb_.empty()) {
        preview = app->latest_preview_rgb_.clone();
      }
    }
    app->preview_update_pending_ = false;

    if (!preview.empty() && !app->stopping_) {
      const gsize byte_count = preview.total() * preview.elemSize();
      gpointer pixels = g_memdup2(preview.data, byte_count);
      GBytes* bytes = g_bytes_new_take(pixels, byte_count);
      GdkPixbuf* pixbuf = gdk_pixbuf_new_from_bytes(
          bytes, GDK_COLORSPACE_RGB, FALSE, 8, preview.cols, preview.rows,
          static_cast<int>(preview.step));
      g_bytes_unref(bytes);

      if (app->preview_pixbuf_) {
        g_object_unref(app->preview_pixbuf_);
      }
      app->preview_pixbuf_ = pixbuf;
      gtk_widget_queue_draw(app->drawing_area_);
    }
    return G_SOURCE_REMOVE;
  }

  static gboolean on_draw(GtkWidget* widget, cairo_t* cairo, gpointer data) {
    auto* app = static_cast<MicroscopeApp*>(data);
    GtkAllocation allocation{};
    gtk_widget_get_allocation(widget, &allocation);

    cairo_set_source_rgb(cairo, 0.0, 0.0, 0.0);
    cairo_paint(cairo);

    if (!app->preview_pixbuf_) {
      return FALSE;
    }

    const int image_width = gdk_pixbuf_get_width(app->preview_pixbuf_);
    const int image_height = gdk_pixbuf_get_height(app->preview_pixbuf_);
    const int rotation = app->rotation_quarters_.load();
    const bool swaps_axes = rotation % 2 != 0;
    const int displayed_width = swaps_axes ? image_height : image_width;
    const int displayed_height = swaps_axes ? image_width : image_height;
    const double scale = std::min(
        static_cast<double>(allocation.width) / displayed_width,
        static_cast<double>(allocation.height) / displayed_height);

    cairo_save(cairo);
    cairo_translate(cairo, allocation.width / 2.0, allocation.height / 2.0);
    cairo_scale(cairo, scale, scale);
    if (app->mirrored_.load()) {
      cairo_scale(cairo, -1.0, 1.0);
    }
    constexpr double kPi = 3.14159265358979323846;
    cairo_rotate(cairo, rotation * kPi / 2.0);
    cairo_translate(cairo, -image_width / 2.0, -image_height / 2.0);
    gdk_cairo_set_source_pixbuf(cairo, app->preview_pixbuf_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BILINEAR);
    cairo_paint(cairo);
    cairo_restore(cairo);
    return FALSE;
  }

  static void on_resolution_changed(GtkComboBox* combo, gpointer data) {
    auto* app = static_cast<MicroscopeApp*>(data);
    if (app->changing_combo_) {
      return;
    }

    gchar* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (!text) {
      return;
    }
    int width = 0;
    int height = 0;
    if (std::sscanf(text, "%d×%d", &width, &height) == 2) {
      {
        std::lock_guard lock(app->config_mutex_);
        app->requested_resolution_ = {width, height};
      }
      app->set_status_ui("Switching to " + std::string(text) + "…", false);
      app->restart_requested_ = true;
    }
    g_free(text);
  }

  static void on_capture_clicked(GtkButton*, gpointer data) {
    static_cast<MicroscopeApp*>(data)->capture_jpg();
  }

  static void on_open_clicked(GtkButton*, gpointer data) {
    static_cast<MicroscopeApp*>(data)->open_captures();
  }

  static void on_reconnect_clicked(GtkButton*, gpointer data) {
    auto* app = static_cast<MicroscopeApp*>(data);
    app->current_device_.clear();
    app->set_status_ui("Rescanning USB devices…", false);
    app->restart_requested_ = true;
  }

  static void on_rotate_clicked(GtkButton*, gpointer data) {
    static_cast<MicroscopeApp*>(data)->rotate_view();
  }

  static void on_mirror_toggled(GtkToggleButton* button, gpointer data) {
    static_cast<MicroscopeApp*>(data)->set_mirrored(
        gtk_toggle_button_get_active(button));
  }

  static gboolean on_key_press(GtkWidget*, GdkEventKey* event, gpointer data) {
    const bool control = event->state & GDK_CONTROL_MASK;
    if (event->keyval == GDK_KEY_space ||
        (control && (event->keyval == GDK_KEY_s || event->keyval == GDK_KEY_S))) {
      static_cast<MicroscopeApp*>(data)->capture_jpg();
      return TRUE;
    }
    if (!control && (event->keyval == GDK_KEY_r || event->keyval == GDK_KEY_R)) {
      static_cast<MicroscopeApp*>(data)->rotate_view();
      return TRUE;
    }
    if (!control && (event->keyval == GDK_KEY_m || event->keyval == GDK_KEY_M)) {
      auto* app = static_cast<MicroscopeApp*>(data);
      gtk_toggle_button_set_active(
          GTK_TOGGLE_BUTTON(app->mirror_button_), !app->mirrored_.load());
      return TRUE;
    }
    return FALSE;
  }

  static void on_window_destroy(GtkWidget*, gpointer data) {
    static_cast<MicroscopeApp*>(data)->stop_worker();
  }
};

std::optional<Resolution> parse_resolution(const char* text) {
  int width = 0;
  int height = 0;
  if (text && std::sscanf(text, "%dx%d", &width, &height) == 2 && width > 0 &&
      height > 0) {
    return Resolution{width, height};
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  bool probe_only = false;
  std::optional<Resolution> initial_resolution;
  std::vector<char*> gtk_arguments;
  gtk_arguments.push_back(argv[0]);

  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--probe") {
      probe_only = true;
    } else if (argument == "--size" && index + 1 < argc) {
      initial_resolution = parse_resolution(argv[++index]);
      if (!initial_resolution) {
        std::fprintf(stderr, "Invalid resolution. Use WIDTHxHEIGHT.\n");
        return 2;
      }
    } else if (argument.rfind("--size=", 0) == 0) {
      initial_resolution = parse_resolution(argument.c_str() + 7);
      if (!initial_resolution) {
        std::fprintf(stderr, "Invalid resolution. Use WIDTHxHEIGHT.\n");
        return 2;
      }
    } else if (argument == "--help" || argument == "-h") {
      std::printf("Usage: amscope-app [--size WIDTHxHEIGHT] [--probe]\n");
      return 0;
    } else {
      gtk_arguments.push_back(argv[index]);
    }
  }

  if (probe_only) {
    auto camera = find_camera();
    if (!camera) {
      std::fprintf(stderr, "%s not found\n", kCameraName);
      return 1;
    }
    std::printf("Device: %s\nSizes:\n", camera->device.c_str());
    for (const auto& resolution : camera->resolutions) {
      std::printf("  %dx%d\n", resolution.width, resolution.height);
    }
    return 0;
  }

  int gtk_argc = static_cast<int>(gtk_arguments.size());
  GtkApplication* application = gtk_application_new(kApplicationId, G_APPLICATION_DEFAULT_FLAGS);
  MicroscopeApp microscope(application, initial_resolution);
  g_signal_connect(application, "activate", G_CALLBACK(+[](GtkApplication*, gpointer data) {
                     static_cast<MicroscopeApp*>(data)->activate();
                   }),
                   &microscope);

  const int status = g_application_run(G_APPLICATION(application), gtk_argc,
                                       gtk_arguments.data());
  microscope.stop_worker();
  g_object_unref(application);
  return status;
}
