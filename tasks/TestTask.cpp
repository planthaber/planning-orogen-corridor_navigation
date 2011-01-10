/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */

#include "TestTask.hpp"

using namespace corridor_servoing;
using namespace Eigen;

struct corridor_servoing::VFHStarTest : public vfh_star::VFHStar
{
    AngleIntervals allowed_windows;

    AngleIntervals getNextPossibleDirections(const base::Pose& current_pose, double safetyDistance, double robotWidth) const
    {

        TreeSearch::AngleIntervals result;
        double heading = TreeSearch::getHeading(current_pose.orientation);
        for (unsigned int i = 0; i < allowed_windows.size(); ++i)
        {
            double from = allowed_windows[i].first + heading;
            double to   = allowed_windows[i].second + heading;

            if (from > 2 * M_PI)
                from -= 2 * M_PI;
            else if (from < 0)
                from += 2 * M_PI;

            if (to > 2 * M_PI)
                to -= 2 * M_PI;
            else if (to < 0)
                to += 2 * M_PI;

            result.push_back( make_pair(from, to) );
        }

        return result;
    }

    base::Pose getProjectedPose(const base::Pose& curPose,
            double heading, double distance) const
    {
        Eigen::Quaterniond q = Quaterniond(AngleAxisd(heading, Vector3d::UnitZ()));
        Eigen::Vector3d p = curPose.position + q * Vector3d::UnitY() * distance;
        return base::Pose(p, q);
    }
};

TestTask::TestTask(std::string const& name, TaskCore::TaskState initial_state)
    : TestTaskBase(name, initial_state)
    , search(new VFHStarTest)
{
}



/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See TestTask.hpp for more detailed
// documentation about them.

// bool TestTask::configureHook()
// {
//     if (! TestTaskBase::configureHook())
//         return false;
//     return true;
// }

bool TestTask::startHook()
{
    if (! TestTaskBase::startHook())
        return false;

    search->allowed_windows.clear();
    vfh_star::TestConfiguration test_conf = _test_conf.get();
    for (unsigned int i = 0; i < test_conf.angular_windows.size() / 2; ++i)
    {
        search->allowed_windows.push_back( make_pair(
            test_conf.angular_windows[i * 2],
            test_conf.angular_windows[i * 2 + 1]));
    }
    
    search->setSearchConfiguration(_search_conf.get());
    search->setCostConfiguration(_cost_conf.get());

    return true;
}

void TestTask::updateHook()
{
    TestTaskBase::updateHook();

    std::vector<base::Waypoint> wp = search->getTrajectory(_initial_pose.get(), _test_conf.get().main_direction);

    std::vector<base::Vector3d> points;
    for (unsigned int i = 0; i < wp.size(); ++i)
        points.push_back(wp[i].position);

    base::geometry::Spline<3> trajectory;
    trajectory.interpolate(points);
    _trajectory.write(trajectory);

    std::cerr << search->getTree().getSize() << " nodes in tree" << std::endl;
    _search_tree.write(search->getTree());
    stop();
}

// void TestTask::errorHook()
// {
//     TestTaskBase::errorHook();
// }
// void TestTask::stopHook()
// {
//     TestTaskBase::stopHook();
// }
// void TestTask::cleanupHook()
// {
//     TestTaskBase::cleanupHook();
// }

