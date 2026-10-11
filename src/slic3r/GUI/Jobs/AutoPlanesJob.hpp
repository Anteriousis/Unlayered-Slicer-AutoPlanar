#ifndef AUTOPLANESJOB_HPP
#define AUTOPLANESJOB_HPP

// UNLAYERED_MOD -- run the automatic plane planner (libslic3r/AutoPlanes) off the UI thread.
//
// Same lifetime model as SkeletonJob: a shared state block the worker fills and the cut-plane gizmo
// polls each frame. The worker gets its OWN copy of the object (a private Model), so the user can
// edit, delete or undo the real object while planning runs; a result that no longer matches the
// selection is dropped when it arrives.

#include "Job.hpp"

#include "libslic3r/AutoPlanes.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r { namespace GUI {

struct AutoPlanesJobState
{
    std::atomic<bool> done{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> failed{false};
    std::atomic<int>  percent{0};

    std::mutex              mtx;   // guards stage/error/result
    std::string             stage;
    std::string             error;
    AutoPlanes::PlanResult  result;

    // Identity of the request: the object and instance it was planned for, and the cut-plane stack
    // it started from. A result is applied only if all three still match.
    ObjectID      source_id;
    int           instance_idx{0};
    CutPlaneStack source_stack;

    std::string stage_copy() { std::lock_guard<std::mutex> lk(mtx); return stage; }
};

using AutoPlanesJobStatePtr = std::shared_ptr<AutoPlanesJobState>;

class AutoPlanesJob : public Job
{
public:
    // `object_copy` must be a Model holding exactly one object -- a deep copy taken on the UI thread.
    AutoPlanesJob(std::unique_ptr<Model> object_copy, Transform3d instance_trafo, DynamicPrintConfig config,
                  AutoPlanes::Options options, AutoPlanesJobStatePtr state)
        : m_model(std::move(object_copy)), m_trafo(instance_trafo), m_config(std::move(config))
        , m_options(options), m_state(std::move(state))
    {}

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    std::unique_ptr<Model> m_model;
    Transform3d            m_trafo;
    DynamicPrintConfig     m_config;
    AutoPlanes::Options    m_options;
    AutoPlanesJobStatePtr  m_state;
};

}} // namespace Slic3r::GUI

#endif // AUTOPLANESJOB_HPP
