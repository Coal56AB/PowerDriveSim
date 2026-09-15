#pragma once
#include "core/editor/document.hpp"
#include "core/solver/reference/reference.hpp"
#include <QFutureWatcher>
#include <QGraphicsView>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <atomic>
#include <memory>
#include <optional>
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
namespace pds::desktop {
QString text(const char *key);
void init_language(const QString &language);
class Canvas : public QGraphicsView {
  public:
    explicit Canvas(QWidget *parent = nullptr);
    std::function<void(Endpoint)> port_clicked;
    std::function<void(QPointF)> place;
    std::function<void()> movement, released;
    std::function<void(std::string)> open_object, observe;
    std::function<void(std::string,QPoint)> context_menu;
    std::function<void(Endpoint,std::optional<Endpoint>,QPointF,std::string)> wire_dropped;
    std::function<bool(const Endpoint&,const Endpoint&)> compatible;
    void cancel_wire();
    void set_ghost(QGraphicsItem* item);
    std::optional<Endpoint> hovered_port() const;

  protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void leaveEvent(QEvent*) override;
    void drawBackground(QPainter *, const QRectF &) override;
    void drawForeground(QPainter *, const QRectF &) override;

  private:
    QGraphicsItem* ghost_=nullptr;
    std::optional<Endpoint> dragging_wire_;
    QPointF wire_origin_;QGraphicsPathItem* wire_preview_=nullptr;
    std::optional<Endpoint> port_at(QPoint point) const;
    bool panning_ = false;
    QPoint pan_origin_, last_mouse_;
};
class Scope : public QWidget {
  public:
    explicit Scope(QWidget *parent = nullptr);
    void set_result(const Result *result, const std::vector<int> &channels, const Project &project);
    void fit();
    std::function<void(double, double, double, double)> changed;
    double begin = 0, end = 1, cursor_a = -1, cursor_b = -1;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    const Result *result_ = nullptr;
    std::vector<int> channels_;
    double sample_value(size_t sample, int channel) const;
};
struct Outcome {
    std::optional<Result> result;
    QString error;
    std::string object;
};
class EditorWindow : public QMainWindow {
  public:
    explicit EditorWindow(const QString &language = "ru", const QString &recovery_dir = {});
    ~EditorWindow() override;
    const Project &project() const { return document_->project(); }
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
    void undo();
    void redo();
    void select_object(const std::string &id);
    void open_plot(const std::string &id);
    void show_shortcuts();
    void set_scope_enabled(bool enabled);
    void observe_object(const std::string &id);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    std::unique_ptr<Document> document_;
    Canvas *canvas_ = nullptr;
    Scope *scope_ = nullptr;
    QTreeWidget *library_ = nullptr;
    QListWidget *objects_ = nullptr, *channels_ = nullptr, *errors_ = nullptr;
    QTabWidget *bottom_ = nullptr;
    QWidget *scope_page_ = nullptr, *scope_content_ = nullptr;
    QVBoxLayout *scope_layout_ = nullptr;
    QCheckBox *scope_enable_ = nullptr;
    QLabel *scope_hint_ = nullptr;
    QPushButton *scope_export_ = nullptr;
    QSpinBox *inputs_ = nullptr;
    std::map<std::string, QPointer<QDialog>> plot_windows_;
    std::map<std::string, QPointer<Scope>> plot_views_;
    QLineEdit *name_ = nullptr, *value_ = nullptr, *initial_ = nullptr, *stop_ = nullptr, *step_ = nullptr;
    QCheckBox *closed_ = nullptr;
    QPlainTextEdit *events_ = nullptr, *bends_ = nullptr;
    QComboBox *method_ = nullptr;
    QFormLayout *properties_ = nullptr;
    QLabel *banner_ = nullptr;
    QLabel *inspector_hint_ = nullptr;
    QPushButton *apply_button_ = nullptr;
    QAction *run_ = nullptr, *stop_action_ = nullptr, *undo_ = nullptr, *redo_ = nullptr;
    QFutureWatcher<Outcome> watcher_;
    std::atomic_bool cancel_{false};
    std::atomic<double> simulated_time_{0};
    std::optional<Result> result_;
    std::optional<Endpoint> pending_port_;
    std::string selected_, saved_state_;
    std::map<std::string,QAction*> commands_;
    std::map<std::string,QKeySequence> default_shortcuts_;
    unsigned paste_count_=0;
    QString path_, recovery_dir_;
    int placing_ = -1;
    bool rebuilding_ = false, busy_ = false;
    std::map<std::string, QGraphicsItem *> atoms_;
    std::map<std::string, QGraphicsPathItem *> wires_;
    void build_ui();
    void set_placement_preview();
    QLineEdit *frequency_=nullptr,*duty_=nullptr,*delay_=nullptr;
    std::vector<std::string> selected_ids() const;
    void transform_selection(int turns,bool mirror);
    void arrange_selection(const std::string& mode);
    bool copy_selection(bool cut);
    void paste_selection(bool duplicate=false);
    void show_context(const std::string& id,QPoint global);
    void load_shortcuts();
    void finish_wire(Endpoint from,std::optional<Endpoint> to,QPointF point,const std::string& wire);
    void refresh(bool invalidate = true);
    void rebuild_scene();
    void update_wires();
    void fill_inspector();
    void apply_inspector();
    void show_error(const std::exception &);
    void handle_port(Endpoint);
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
