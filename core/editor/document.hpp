#pragma once
#include "core/model/connectivity.hpp"
#include <functional>
namespace pds {
class Document {
public:
    explicit Document(Project project);
    const Project& project() const { return current_; }
    void apply(const std::string& label,const std::function<void(Project&)>& change);
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    void undo();
    void redo();
    std::string add_component(Kind kind,double x,double y);
    std::string add_node(bool ground,double x,double y);
    std::string add_pattern(double x,double y);
    std::string add_plot(double x,double y,const std::string& name="Plot");
    void connect(Endpoint from,Endpoint to);
    Project copy(const std::vector<std::string>& ids) const;
    std::vector<std::string> paste(const Project& fragment,double dx,double dy);
    void transform(const std::vector<std::string>& ids,int quarter_turns,bool mirror);
    void arrange(const std::vector<std::string>& ids,const std::string& mode);
    void erase(const std::vector<std::string>& ids);
private:
    struct Change { std::string label; Project before,after; };
    Project current_;
    std::vector<Change> undo_,redo_;
};
}
