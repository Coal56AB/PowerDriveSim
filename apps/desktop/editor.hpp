#pragma once
#include "core/editor/document.hpp"
#include "core/solver/reference/reference.hpp"
#include "results/measurements.hpp"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGraphicsView>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
class QListWidget;
class QTreeWidget;
class QTabWidget;
class QVBoxLayout;
class QSpinBox;
class QDialog;
class QPushButton;
class QLineEdit;
class QPlainTextEdit;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QGraphicsPathItem;
class QLabel;
class QAction;
class QTableWidget;
class QToolBar;
class QMenu;
namespace pds::desktop {
inline constexpr int wire_segment_role = 3; // 1-based path edge; zero selects the whole wire.
inline constexpr int channel_highlight_role = 6;
QString text(const char *key);
QString engineering_value(double value, const std::string &unit);
bool property_visible(const Project &project,const std::string &id,const QJsonObject &field);
void init_language(const QString &language);
class Canvas : public QGraphicsView {
  public:
    explicit Canvas(QWidget *parent = nullptr);
    std::function<void(QPointF)> place;
    std::function<void()> movement, released;
    std::function<void(std::string)> open_object;
    std::function<void(std::string, QPoint)> context_menu;
    std::function<bool(const Endpoint &, const Endpoint &)> compatible;
    std::function<QPainterPath(Endpoint, std::optional<Endpoint>, QPointF, QPointF)> route_preview;
    std::function<Endpoint(const std::string &, bool)> wire_endpoint;
    std::function<void(WireAnchor, WireAnchor, std::vector<Point>, std::string)> connect_wire;
    std::function<void(std::string, std::vector<Point>)> edit_route;
    std::function<void(QPointF)> quick_insert;
    std::function<bool(QPoint)> edit_text;
    std::function<void()> cancel_placement;
    void cancel_wire();
    void cancel_gesture();
    void set_editable(bool enabled);
    bool editing_gesture() const;
    bool transform_move(int turns, bool mirror);
    Point moving_point(const std::string &from, const std::string &to, Point point) const;
    QPointF insertion_position() const;
    void set_ghost(QGraphicsItem *item);
    std::optional<Endpoint> hovered_port() const;

  protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void leaveEvent(QEvent *) override;
    void drawBackground(QPainter *, const QRectF &) override;
    void drawForeground(QPainter *, const QRectF &) override;
    bool viewportEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    enum class Gesture { idle, selecting, moving, placing, wiring, routing, reconnecting, panning };
    Gesture gesture_ = Gesture::idle, resume_ = Gesture::idle;
    bool editable_ = true, dragged_ = false;
    QGraphicsItem *ghost_ = nullptr;
    WireAnchor source_;
    std::string edited_wire_;
    std::vector<QPointF> original_route_;
    int segment_ = 0, vertex_ = -1;
    QGraphicsPathItem *route_item_ = nullptr;
    std::map<QGraphicsItem *, QPointF> positions_;
    std::map<QGraphicsItem *, QTransform> transforms_;
    QGraphicsItem *move_anchor_ = nullptr;
    QTransform move_transform_, move_base_;
    std::vector<QLineF> guides_;
    QPoint press_;
    QPointF press_scene_;
    QTimer scroll_timer_;
    void move_gesture(QPoint point, Qt::KeyboardModifiers modifiers);
    WireAnchor anchor_at(QPoint point, bool compatible_only) const;
    QGraphicsPathItem *wire_at(QPoint point) const;
    QGraphicsItem *object_at(QPoint point) const;
    void begin_wire(WireAnchor source);
    QPointF wire_origin_;
    QGraphicsPathItem *wire_preview_ = nullptr;
    std::optional<Endpoint> port_at(QPoint point) const;
    QPoint pan_origin_, last_mouse_;
};
class Scope : public QWidget {
  public:
    enum class Axes { x, y, xy };
    explicit Scope(QWidget *parent = nullptr);
    void set_result(const Result *result, const std::vector<int> &channels, const Project &project);
    void fit(Axes axes = Axes::xy);
    QWidget *navigation();
    QWidget *channel_controls();
    void set_channel_visible(const std::string &key, bool visible);
    bool channel_visible(const std::string &key) const;
    CurveStyle curve_style(const std::string &key) const;
    QString curve_name(const std::string &key) const;
    void set_curve_name(const std::string &key, const QString &name);
    void set_curve_style(const CurveStyle &style);
    void show_curve_settings(const std::string &key);
    void set_wheel_modifiers(Qt::KeyboardModifiers x, Qt::KeyboardModifiers y) {
        wheel_x_ = x;
        wheel_y_ = y;
    }
    void set_cursor_mode(bool enabled);
    void set_cursor(int index, double time);
    void select_cursor_channel(int index, const std::string &key);
    QString cursor_readout() const;
    void set_live(bool live);
    void set_time_span(double seconds);
    void show_measurements();
    void show_display_settings();
    ViewOptions view_options() const;
    void refresh_theme();
    void load_view_options(const ViewOptions &options);
    std::optional<ViewOptions> pending_options_;
    Axes zoom_axes = Axes::x;
    double y_low = -1, y_high = 1;
    std::function<void(double, double, double, double)> changed;
    double begin = 0, end = 1, cursor_a = -1, cursor_b = -1;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool event(QEvent *) override;

  private:
    const Result *result_ = nullptr;
    std::vector<int> channels_;
    std::set<std::string> hidden_channels_;
    QPointer<QToolBar> channel_bar_;
    void update_channel_controls();
    std::vector<CurveStyle> curve_styles_;
    std::vector<std::pair<std::string, std::string>> curve_names_;
    std::vector<LegendPosition> legend_positions_;
    struct LegendLayout {
        QRectF box;
        std::vector<std::pair<int, QRectF>> rows;
    };
    LegendLayout legend_layout(int lane) const;
    void paint_legend(QPainter &painter, int lane);
    int legend_at(QPointF point) const;
    int legend_drag_lane_ = -1;
    std::optional<LegendPosition> legend_drag_original_;
    QRectF legend_drag_box_;
    bool legend_drag_moved_ = false;
    void move_legend(QPointF point);
    Qt::MouseButton drag_button_ = Qt::NoButton;
    QPointF drag_origin_;
    double drag_begin_ = 0, drag_end_ = 0;
    double drag_low_ = 0, drag_high_ = 0;
    QPointF drag_current_;
    enum class NavigationTool { pan, zoom, cursors };
    NavigationTool navigation_tool_ = NavigationTool::zoom;
    void set_navigation_tool(NavigationTool tool);
    bool selecting_zoom_ = false;
    bool live_ = false, follow_live_ = true;
    int active_cursor_ = 0;
    Qt::KeyboardModifiers wheel_x_ = Qt::ControlModifier, wheel_y_ = Qt::ShiftModifier;
    std::string cursor_channels_[2];
    QPointer<QWidget> cursor_panel_;
    QPointer<QComboBox> cursor_choices_[2];
    QPointer<QLineEdit> cursor_times_[2];
    QPointer<QLabel> cursor_values_[2], cursor_math_;
    QPointer<QAction> cursor_action_, zoom_actions_[3];
    QPointer<QPushButton> cursor_buttons_[2];
    bool screen_cursors_ = false, show_grid_ = true, show_legend_ = false, separate_axes_ = false;
    double cursor_y_[2] = {0, 0}, time_span_ = 0, line_width_ = 1.8;
    double drag_cursors_[4] = {-1, -1, 0, 0};
    std::vector<std::pair<double, double>> display_ranges_;
    int active_lane_ = 0, display_columns_ = 1;
    std::vector<std::pair<std::string, unsigned>> signal_displays_;
    int display_count() const;
    int channel_display(int channel) const;
    QPointer<QDialog> measurements_;
    QPointer<QAction> follow_action_;
    QPointer<QLineEdit> time_span_edit_;
    std::string trigger_channel_;
    bool trigger_armed_ = false;
    int trigger_edge_ = 0, trigger_mode_ = 0;
    double trigger_level_ = 0, trigger_after_ = 0, trigger_holdoff_ = 0, trigger_position_ = .2;
    std::optional<double> trigger_time_;
    struct ViewRange {
        double begin, end, low, high;
        std::vector<std::pair<double, double>> lanes;
    };
    std::vector<ViewRange> view_history_;
    size_t view_index_ = 0;
    void remember_view();
    void restore_view(int direction);
    void update_measurements();
    void update_live_view(bool force = false);
    std::optional<CursorValue> cursor_reading(int index) const;
    QRectF lane_rect(int lane) const;
    bool activate_lane(QPointF point);
    QRectF plot_rect() const;
    void notify_view();
    void update_cursor_panel();
    void populate_cursor_channels();
    int cursor_channel(int index) const;
    void cancel_drag();
    void fit_y();
    double sample_value(size_t sample, int channel) const;
};
struct Outcome {
    std::optional<Result> result;
    QString error;
    std::string object;
    std::vector<std::string> path;
    double preparation_seconds = 0, execution_seconds = 0;
};
class EditorWindow : public QMainWindow {
  public:
    explicit EditorWindow(const QString &language = "ru", const QString &recovery_dir = {});
    ~EditorWindow() override;
    const Project &project() const { return document_->project(); }
    const Project &root_project() const { return document_->root_project(); }
    const std::vector<std::string> &hierarchy_path() const { return document_->location(); }
    void open_subcircuit(const std::string &id);
    void navigate_hierarchy(const std::vector<std::string> &path);
    std::string group_selection(const QString &name);
    void detach_selected();
    void expand_selected();
    Canvas *canvas() const { return canvas_; }
    Scope *scope() const { return scope_; }
    bool running() const { return busy_; }
    bool has_result() const { return result_.has_value(); }
    const Result &result() const { return *result_; }
    void set_project(Project project);
    bool open_project(const QString &path);
    bool save_project(const QString &path);
    bool autosave();
    bool recover(const QString &path);
    QString recovery_path() const;
    std::string add_component(Kind kind, QPointF point);
    std::string add_node(bool ground, QPointF point);
    std::string add_pattern(QPointF point);
    std::string add_plot(QPointF point);
    bool connect_ports(Endpoint from, Endpoint to);
    QPointF port_position(const Endpoint &endpoint) const;
    void start_simulation();
    void stop_simulation();
    void continue_simulation();
    void step_simulation();
    bool save_simulation_snapshot(const QString &path);
    bool load_simulation_snapshot(const QString &path);
    void show_initial_settings();
    const std::optional<SimulationSnapshot> &simulation_snapshot() const { return continuation_; }
    void undo();
    void redo();
    void select_object(const std::string &id);
    void open_plot(const std::string &id);
    void show_shortcuts();
    void set_scope_enabled(bool enabled);
    void observe_object(const std::string &id);

  protected:
    void closeEvent(QCloseEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    std::unique_ptr<Document> document_;
    QLabel *breadcrumbs_ = nullptr;
    QTreeWidget *hierarchy_ = nullptr;
    std::vector<std::string> scene_path_;
    bool hierarchy_edit_enabled_ = false;
    bool editing_allowed() const {
        return !running() && (hierarchy_path().empty() || hierarchy_edit_enabled_);
    }
    void build_hierarchy_actions(QMenu *menu);
    void refresh_hierarchy();
    void update_instance_specs();
    void edit_public_interface(const std::string &definition);
    std::vector<std::string> visible_plot_channels(const std::string &id) const;
    Canvas *canvas_ = nullptr;
    Scope *scope_ = nullptr;
    QTreeWidget *library_ = nullptr;
    QToolBar *component_bar_ = nullptr;
    std::map<int, QAction *> component_actions_;
    std::map<std::string, QJsonObject> component_specs_;
    std::map<QString, QWidget *> property_editors_;
    std::map<QString, QPushButton *> property_imports_;
    QJsonArray active_fields_;
    QPointer<QLineEdit> inline_editor_;
    std::vector<int> pinned_components_;
    QListWidget *objects_ = nullptr, *channels_ = nullptr, *errors_ = nullptr;
    QTabWidget *bottom_ = nullptr;
    QWidget *scope_page_ = nullptr, *scope_content_ = nullptr;
    QVBoxLayout *scope_layout_ = nullptr;
    QCheckBox *scope_enable_ = nullptr;
    QLabel *scope_hint_ = nullptr;
    QPushButton *scope_export_ = nullptr, *scope_fit_ = nullptr;
    std::map<std::string, QPointer<QDialog>> plot_windows_;
    std::map<std::string, QPointer<Scope>> plot_views_;
    QLineEdit *name_ = nullptr, *value_ = nullptr, *stop_ = nullptr, *step_ = nullptr;
    QComboBox *method_ = nullptr;
    QFormLayout *properties_ = nullptr;
    QLabel *banner_ = nullptr;
    QLabel *inspector_hint_ = nullptr;
    QPushButton *apply_button_ = nullptr;
    QAction *run_ = nullptr, *stop_action_ = nullptr, *undo_ = nullptr, *redo_ = nullptr;
    QFutureWatcher<Outcome> watcher_;
    std::atomic_bool cancel_{false};
    std::atomic_bool paused_{false};
    std::mutex stream_mutex_;
    std::deque<Result> stream_queue_;
    void drain_simulation_stream();
    void append_simulation_result(Result result);
    void update_run_button();
    void launch_simulation(std::optional<SimulationSnapshot> state, size_t max_steps = 0);
    std::optional<SimulationSnapshot> continuation_;
    size_t continuation_steps_ = 0;
    std::atomic<double> simulated_time_{0};
    std::atomic_bool preparing_{false};
    QElapsedTimer simulation_timer_;
    std::optional<Result> result_;
    std::optional<Project> result_project_, scene_project_;
    std::map<std::string, std::string> net_cache_;
    std::map<std::string, PortType> port_types_;
    std::map<std::string, ObjectPath> source_paths_;
    std::map<std::string, std::string> signal_sources_;
    std::map<std::string, QPointF> route_positions_;
    std::map<std::string, QRectF> obstacle_cache_;
    bool topology_dirty_ = true;
    std::string selected_, saved_state_;
    std::map<std::string, QAction *> commands_;
    std::map<std::string, QKeySequence> default_shortcuts_;
    QString path_, recovery_dir_;
    Qt::KeyboardModifiers scope_wheel_x_ = Qt::ControlModifier, scope_wheel_y_ = Qt::ShiftModifier;
    void update_scope_shortcuts();
    int placing_ = -1;
    std::optional<Project> paste_fragment_;
    bool rebuilding_ = false, busy_ = false;
    std::map<std::string, QGraphicsItem *> atoms_;
    std::map<std::string, QGraphicsItem *> labels_;
    void update_labels();
    bool commit_label_positions();
    bool transform_labels(int turns, bool mirror);
    std::map<std::string, QGraphicsPathItem *> wires_;
    std::map<std::string, QPainterPath> base_wire_routes_;
    void share_wire_trunks(const std::vector<QRectF> &obstacles, const std::set<std::string> &networks);
    void build_ui();
    void load_component_specs();
    void build_property_editors();
    bool edit_text_at(QPoint position);
    void cancel_inline_edit();
    bool commit_inline_edit();
    void edit_inline(const std::string &id, const QJsonObject &field, QRect rect);
    void build_component_palette(QLineEdit *search);
    void rebuild_component_bar();
    void refresh_component_icons();
    void set_component_pinned(int id, bool pinned);
    void begin_placement(int id);
    void set_placement_preview();
    QGraphicsItem *make_atom_preview(const Project &fragment);
    void quick_insert(QPointF position);
    void cancel_placement();
    void place_at(QPointF point);
    void connect_gesture(WireAnchor from, WireAnchor to, std::vector<Point> bends, std::string replace);
    QPointF port_stub(const Endpoint &endpoint, QPointF point) const;
    QPainterPath preview_route(Endpoint from, std::optional<Endpoint> to, QPointF start, QPointF end) const;
    void commit_profile();
    void remember_draft();
    void update_command_state();
    QLabel *property_error_ = nullptr;
    bool inspector_loading_ = false, applying_ = false;
    std::map<std::string, QMap<QString, QString>> drafts_;
    std::string inspector_id_;
    std::vector<std::string> selected_ids() const;
    void transform_selection(int turns, bool mirror);
    void arrange_selection(const std::string &mode);
    bool copy_selection(bool cut);
    void paste_selection(bool duplicate = false);
    void show_context(const std::string &id, QPoint global);
    void load_shortcuts();
    void refresh(bool invalidate = true);
    void rebuild_scene();
    void update_wires();
    void fill_inspector();
    void apply_inspector();
    void import_samples(const QString &key);
    void show_error(const std::exception &);
    void commit_positions();
    void delete_selected();
    void choose_channels();
    void finish_simulation();
    void update_title();
    bool confirm_discard();
    void new_file();
    void export_csv(const std::vector<std::string> &keys);
    void sync_scope();
    void refresh_channel_catalog();
    void update_graphs();
    void clear_result();
    std::vector<std::string> recording_keys() const;
    std::vector<int> result_indices(const std::vector<std::string> &keys) const;
};
} // namespace pds::desktop
