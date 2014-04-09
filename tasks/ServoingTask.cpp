#include "ServoingTask.hpp"
#include <vfh_star/VFHStar.h>
#include <vfh_star/VFH.h>
#include <envire/Orocos.hpp>
#include <cmath>
#include <base/Float.hpp>

using namespace corridor_navigation;
using namespace trajectory_follower;
using namespace vfh_star;
using namespace Eigen;

ServoingTask::ServoingTask(std::string const& name)
    : ServoingTaskBase(name), 
            gotNewMap(false), noTrCounter(0), failCount(0), unknownTrCounter(0), 
            unknownRetryCount(0), env(), gridPos(NULL), trGrid(NULL), trFollower(0,0, 0)
{   
}

ServoingTask::~ServoingTask() {}


/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See ServoingTask.hpp for more detailed
// documentation about them.

bool ServoingTask::configureHook()
{    
    if (!ServoingTaskBase::configureHook())
        return false;

    vfhServoing.setCostConf(_cost_conf.get());
    vfhServoing.setSearchConf(_search_conf.get());
    vfhServoing.setAllowBackwardDriving(_allowBackwardsDriving.get());
    
    failCount = _fail_count.get();
    unknownRetryCount = _unknown_retry_count.get();
    minDriveProbability = _minDriveProbability.get();

    trFollower.setForwardLength(_search_horizon.get());
    trFollower.removeTrajectory();

    //aktivate output of debug tree
    vfhServoing.activateDebug();
    
    return true;
}

void ServoingTask::transformationCallback(const base::Time& ts, transformer::Transformation& tr, Eigen::Affine3d& value, bool& gotIt)
{
    if(!tr.get(ts, value, false))
        throw std::runtime_error("Fatal error in transformation stack");
    
    gotIt = true;
}


bool ServoingTask::startHook()
{  
    if(!ServoingTaskBase::startHook())
	return false;
    
    gotNewMap = false;
    hasHeading_map = false;
    noTrCounter = 0;
    unknownTrCounter = 0;
    gotBodyCenter2Map = false;
    gotBodyCenter2Trajectory = false;
    gotBodyCenter2GlobalTrajectory = false;
    didConsistencySweep = false;
    
    sweepTracker.reset();

    _body_center2map.registerUpdateCallback(
        boost::bind(&ServoingTask::transformationCallback , this, _1, boost::ref(_body_center2map), boost::ref(bodyCenter2Map), boost::ref(gotBodyCenter2Map)));
    _body_center2trajectory.registerUpdateCallback(
        boost::bind(&ServoingTask::transformationCallback , this, _1, boost::ref(_body_center2trajectory), boost::ref(bodyCenter2Trajectory), boost::ref(gotBodyCenter2Trajectory)));
    _body_center2global_trajectory.registerUpdateCallback(boost::bind(&ServoingTask::bodyCenter2GlobalTrajectoryCallback , this, _1));
    
    return true;
}

void ServoingTask::bodyCenter2GlobalTrajectoryCallback(const base::Time& ts)
{
    if(!_body_center2global_trajectory.get(ts, bodyCenter2GlobalTrajectorie, false))
    {
        throw std::runtime_error("Fatal error in transformation stack");
    }
    
    gotBodyCenter2GlobalTrajectory = true;
    
    //needed for heading transformation
    if(!gotBodyCenter2Map)
        return;
    
    hasHeading_map = getDriveDirection(heading_map);
}


bool ServoingTask::getDriveDirection(base::Angle &result)
{
    base::Pose curPose(bodyCenter2GlobalTrajectorie);
    
    Eigen::Vector2d motionCmd;
    TrajectoryFollower::FOLLOWER_STATUS status = 
            trFollower.traverseTrajectory(motionCmd, base::Pose(bodyCenter2GlobalTrajectorie));
                
    base::Waypoint goalPoint;
    
    switch(status)
    {
        case TrajectoryFollower::REACHED_TRAJECTORY_END:
            if(!trajectories.empty())
            {
                    if(state() != RUNNING)
                        state(RUNNING);
                    trFollower.setNewTrajectory(trajectories.front());
                    trajectories.erase(trajectories.begin());
                    //recursive call for next part of trajectorie
                    return getDriveDirection(result);
            }
            else
            {
                if(state() != REACHED_END_OF_TRAJECTORY)
                {
                    goalPoint = base::Waypoint(trFollower.getCurvePoint().pose.position, trFollower.getCurvePoint().pose.heading, 0.01, 0.0);
                    _targetPointOnGlobalTrajectory.write(goalPoint);
                    RTT::log(RTT::Info) << "End of the trajectory reached" << RTT::endlog();
                    state(REACHED_END_OF_TRAJECTORY);
                }
                return false;
            }
            break;
        case TrajectoryFollower::INITIAL_STABILITY_FAILED:
            //we don't care about initial stability, as we don't use the motion command fromt eh tr follower
        case TrajectoryFollower::RUNNING:
            goalPoint = base::Waypoint(trFollower.getCurvePoint().pose.position, trFollower.getCurvePoint().pose.heading, 0.5, 0.0);
            _targetPointOnGlobalTrajectory.write(goalPoint);
            if(state() != RUNNING)
                state(RUNNING);
            break;
    }
    
    //we can't use the motion command of the tr follower. It is in rad/seconds,
    //but we need a target direction, so we calculate it now from the goal pos of the tr follower
    
    //convert goal point into map coordinates
    Eigen::Affine3d globalTrajectory2Map(bodyCenter2Map * bodyCenter2GlobalTrajectorie.inverse());
    
    Vector3d goal_map = globalTrajectory2Map * goalPoint.position;
    Vector3d vetToGoal_map = goal_map - bodyCenter2Map.translation();
    vetToGoal_map.normalize();
    
    // Calculate rotation angle in radians between the x-axis and the goal vector.
    double angleToGoal_map = acos(vetToGoal_map.dot(Eigen::Vector3d::UnitX()));
    if(vetToGoal_map.y() < 0) {
        angleToGoal_map *= -1;
    }
    
    //note we need the direction in the map coorindate system
    result = base::Angle::fromRad(angleToGoal_map);
    return true;
}

void ServoingTask::consistencyCallback(size_t x, size_t y, double& sum, int& cnt)
{
    sum += trGrid->getProbability(x, y);
    cnt++;
}

bool ServoingTask::isMapConsistent()
{
    if(!hasHeading_map)
    {
        return false;
    }

    double forwardDistance = 1.0;
    Vector3d rectangleCenter = bodyCenter2Map.translation() + bodyCenter2Map.linear() * Vector3d(forwardDistance, 0,0);
    
    base::Pose2D rectCenter(Vector2d(rectangleCenter.x(), rectangleCenter.y()), heading_map.getRad());
    
    double sum = 0;
    int cnt = 0;
    
    //check for 1 meter into drive direction
    trGrid->forEachInRectangle(rectCenter, forwardDistance, forwardDistance, boost::bind(&ServoingTask::consistencyCallback, this, _1, _2, boost::ref(sum), boost::ref(cnt)));
    
    double meanProbability = sum / cnt;
    
    if(meanProbability < minDriveProbability)
        return false;
    
    return true;
}


bool ServoingTask::doPathPlanning()
{
     //wait for the sweep to finish before we do a replan
    if(!sweepTracker.areSweepsDone())
    {
        RTT::log(RTT::Info) << "Waiting for sweep to finish" << RTT::endlog();
        return false;
    }   
    
    RTT::log(RTT::Info) << "Trying to plan" << RTT::endlog();

    RTT::log(RTT::Info) << "" << RTT::endlog(); 
    base::Time start = base::Time::now();

    std::vector<base::Trajectory> plannedTrajectory;
    
    if(!hasHeading_map)
    {
        return false;
    }
    
    std::cout << "Doing it, I am planning !" << std::endl;
    Eigen::Affine3d map2Trajectory(bodyCenter2Trajectory * bodyCenter2Map.inverse());
    
    VFHServoing::ServoingStatus status = vfhServoing.getTrajectories(plannedTrajectory, base::Pose(bodyCenter2Map), heading_map, _search_horizon.get(), map2Trajectory, _min_trajectory_lenght.get());
    base::Time end = base::Time::now();

    RTT::log(RTT::Info) << "vfh took " << (end-start).toMicroseconds() << RTT::endlog(); 

    if (_debugVfhTree.connected()) {
        const vfh_star::DebugTree *dTree = vfhServoing.getDebugTree();
        if(dTree)
            _debugVfhTree.write(*dTree);
    }

    if(_horizonDebugData.connected())
    {
        std::cout << "Writing data " << std::endl;
        _horizonDebugData.write(vfhServoing.getDebugData());
    }
    
    //write the trajectory. It is allways valid
    _trajectory.write(plannedTrajectory);
    
    switch(status)
    {
        case VFHServoing::TRAJECTORY_THROUGH_UNKNOWN:
            noTrCounter = 0;
            unknownTrCounter++;
            sweepTracker.triggerSweepTracking();
            if(unknownTrCounter > unknownRetryCount)
            {
                RTT::log(RTT::Error) << "Quitting, trying to drive through unknown terrain" << RTT::endlog();
                if(_allow_exception.get())
                    exception(TRAJECTORY_THROUGH_UNKNOWN);
            }
            break;
        case VFHServoing::NO_SOLUTION:
            unknownTrCounter = 0;
            noTrCounter++;
            sweepTracker.triggerSweepTracking();
            if(noTrCounter > failCount) {
                RTT::log(RTT::Error) << "Quitting, found no solution" << RTT::endlog();
                if(_allow_exception.get())
                    exception(NO_SOLUTION);
            }

            break;
        case VFHServoing::TRAJECTORY_OK:
            unknownTrCounter = 0;
            noTrCounter = 0;
            return true;
            break;
    };
    
    return false;
}

bool ServoingTask::getMap()
{
    //receive map
    envire::OrocosEmitter::Ptr binaryEvents;
    RTT::FlowStatus mapStatus = _map.readNewest(binaryEvents, false);
    if(mapStatus == RTT::NoData)
    {
        return false;
    }
    
    if(mapStatus == RTT::NewData)
    {
        env.applyEvents(*(binaryEvents));

        std::vector<envire::TraversabilityGrid *> trMaps = env.getItems<envire::TraversabilityGrid>();
        if(!trMaps.size() || trMaps.size() > 1) {
            throw std::runtime_error("ServoingTask::Environment contains more than one TraversabilityGrid");
        }
        
        trGrid = *(trMaps.begin());
        gridPos = trGrid->getFrameNode();
        if(!gridPos)
            throw std::runtime_error("ServoingTask::Error, grid has no framenode");
        
        vfhServoing.setNewTraversabilityGrid(trGrid);
        
        gotNewMap = true;
    }
    
    return true;
}

bool ServoingTask::getGlobalTrajectory()
{
    RTT::FlowStatus trStatus = _global_trajectory.readNewest(trajectories, false);
    if(trStatus == RTT::NewData)
    {
        //wait until next position reading comes in
        hasHeading_map = false;
        if(trajectories.empty())
        {
            trFollower.removeTrajectory();
            _trajectory.write(std::vector<base::Trajectory>());
        }
        else
        {
            std::cout << "ServoingTask::Got new Trajectory" << std::endl;
            trFollower.setNewTrajectory(trajectories.front());
            trajectories.erase(trajectories.begin());
        }
    }
    return trStatus != RTT::NoData;
}



void ServoingTask::updateHook()
{
    ServoingTaskBase::updateHook();
    
    tilt_scan::SweepStatus swStatus;
    while(_sweep_status.read(swStatus, false) == RTT::NewData)
    {
        sweepTracker.updateTracker(swStatus);
    }
    
    if(!gotBodyCenter2Map || !gotBodyCenter2Trajectory || !gotBodyCenter2GlobalTrajectory)
    {
        RTT::log(RTT::Debug) << "Waiting for needed transformations" << RTT::endlog();
        return;        
    }
    
    if(!getMap() || !getGlobalTrajectory())
    {
        //no map or goal, stop and do nothing
        _trajectory.write(std::vector<base::Trajectory>());
        RTT::log(RTT::Info) << "No map or trajectory available, stop robot by writing an empty trajectory" << RTT::endlog();
        return;
    }

    //do not plan if nobody listens to us
    if(!_trajectory.connected())
    {
        RTT::log(RTT::Debug) << "Trajectory port not connected, not planning " << RTT::endlog();
        return;
    }
    
    //check if we actually want to replan
    //TODO add only plan every X cm
    if((base::Time::now() - lastSuccessfullPlanning).toSeconds() > _replanning_delay.get())
    {
        //test for map consistency
        if(!didConsistencySweep && !isMapConsistent())
        {
            //if it is inconsistent, sweep once
            sweepTracker.triggerSweepTracking();
        }
        
        if(!sweepTracker.areSweepsDone())
            return;

        if(!doPathPlanning())
            return;

        didConsistencySweep = false;
        lastSuccessfullPlanning = base::Time::now();
    }        
}

// void ServoingTask::errorHook()
// {
// }
void ServoingTask::stopHook()
{
    //write empty trajectory to stop robot
    _trajectory.write(std::vector<base::Trajectory>());
    RTT::log(RTT::Info) << "Write empty trajectory to stop the robot" << RTT::endlog(); 
    ServoingTaskBase::stopHook();
}
// void ServoingTask::cleanupHook()
// {
// }

