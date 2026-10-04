#ifndef slic3r_NonplanarSliceJob_hpp_
#define slic3r_NonplanarSliceJob_hpp_

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "Job.hpp"
#include "slic3r/Utils/NonplanarSliceClient.hpp"

namespace Slic3r {
namespace GUI {

// Hands an already-exported plate to an external nonplanar slicing service and opens the G-code
// it returns. The protocol is in NONPLANAR_SLICE_SERVICE.md; the client does the talking.
//
// The export has to happen before this job starts -- it touches the model and the plate list, so
// it belongs on the GUI thread (see Plater::slice_nonplanar). From here on it is submit, poll,
// download: all blocking, all on the worker thread, with the result opened in finalize() where
// the GUI thread is ours again.
class NonplanarSliceJob : public Job
{
public:
    NonplanarSliceJob(std::string             service_url,
                      boost::filesystem::path mesh,
                      boost::filesystem::path settings,
                      std::string             printer,
                      std::string             process,
                      std::string             filament,
                      boost::filesystem::path out_dir);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &e) override;

    int status_range() const { return 100; }

private:
    std::string             m_url;
    // Both are couriers, written by Plater::slice_nonplanar and deleted when the job ends.
    boost::filesystem::path m_mesh;      // the exported plate
    boost::filesystem::path m_settings;  // the resolved print configuration, as JSON
    std::string             m_printer;
    std::string             m_process;
    std::string             m_filament;
    boost::filesystem::path m_out_dir;

    // Filled by process(), consumed by finalize() on the GUI thread.
    boost::filesystem::path  m_gcode;
    std::string              m_error;
    std::vector<std::string> m_warnings;
    bool                     m_canceled = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_NonplanarSliceJob_hpp_
