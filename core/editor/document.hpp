#pragma once
#include "core/model/connectivity.hpp"
#include <functional>
namespace pds {
struct WireAnchor {
    Endpoint endpoint;
    std::string wire;
    Point point;
    std::vector<Point> route;
};
class Document {
public:
    explicit Document(Project project);
    const Project& project() const { return location_.empty()?current_:view_; }
    const Project& root_project() const { return current_; }
    const std::vector<std::string>& location() const { return location_; }
    std::string current_definition() const;
    void navigate(const std::vector<std::string>& path);
    void apply(const std::string& label,const std::function<void(Project&)>& change);
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    void undo();
    void redo();
    // Viewing results is persisted, but is independent of the model command history.
    void set_view(const std::string& plot, double begin, double end, double a, double b);
    void set_view_options(const ViewOptions& options);
    std::string add_component(Kind kind,double x,double y);
    std::string add_node(bool ground,double x,double y);
    std::string add_pattern(double x,double y);
    std::string add_plot(double x,double y,const std::string& name="Plot");
    void connect(Endpoint from,Endpoint to);
    void connect_anchors(WireAnchor from, WireAnchor to, const std::vector<Point>& bends,
                         const std::string& replace_wire = "", bool replace_gate_driver = false);
    Project copy(const std::vector<std::string>& ids) const;
    std::vector<std::string> paste(const Project& fragment,double dx,double dy);
    void transform(const std::vector<std::string>& ids,int quarter_turns,bool mirror);
    void arrange(const std::vector<std::string>& ids,const std::string& mode);
    void erase(const std::vector<std::string>& ids);
    void remove_junction(const std::string& id, std::vector<Point> first_route, std::vector<Point> second_route);
    std::string create_definition(const std::vector<std::string>& ids,const std::string& name);
    std::string add_instance(const std::string& definition,double x,double y);
    void edit_definition(const std::string& id,const std::function<void(Definition&)>& change);
    void detach_instance(const std::string& id);
    void expand_instance(const std::string& id);
private:
    struct Change { std::string label; Project before,after; std::vector<std::string> before_path,after_path; };
    Project current_,view_;
    std::vector<std::string> location_;
    void rebuild_view();
    Project merge_view(const Project& view) const;
    void apply_with_root(const std::string& label,const std::function<void(Project&)>& change,
                         const std::function<void(Project&)>& finalize);
    std::vector<Change> undo_,redo_;
};
bool same_simulation(const Project& a, const Project& b);
}
