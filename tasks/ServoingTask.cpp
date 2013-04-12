#include "ServoingTask.hpp"
#include <vfh_star/VFHStar.h>
#include <vfh_star/VFH.h>
#include <envire/maps/MLSGrid.hpp>
#include <cmath>

using namespace corridor_navigation;
using namespace vfh_star;
using namespace Eigen;

ServoingTask::ServoingTask(std::string const& name)
    : ServoingTaskBase(name)
{
    globalHeading = 0;
    
    gridPos = new envire::FrameNode();
    env.attachItem(gridPos);
    
    mapGenerator = new vfh_star::TraversabilityMapGenerator();

    const TraversabilityGrid &trGridGMS(mapGenerator->getTraversabilityMap());

    trGrid = new envire::Grid<Traversability>(trGridGMS.getWidth(), trGridGMS.getHeight(), trGridGMS.getGridEntrySize(), trGridGMS.getGridEntrySize());
    env.attachItem(trGrid);
    trGrid->setFrameNode(gridPos);

    copyGrid();

    vfhServoing = new corridor_navigation::VFHServoing();
    vfhServoing->setNewTraversabilityGrid(trGrid);
    gotNewMap = false;
    
    bodyCenter2Odo = Affine3d::Identity();
    
    dynamixelMin = std::numeric_limits< double >::max();
    dynamixelMax = -std::numeric_limits< double >::max();

    vfh_star::TreeSearchConf search_conf;
    corridor_navigation::VFHServoingConf cost_conf;
}

ServoingTask::~ServoingTask() {}

void ServoingTask::copyGrid() 
{
    const TraversabilityGrid &trGridGMS(mapGenerator->getTraversabilityMap());
    envire::Grid<Traversability>::ArrayType &trData = trGrid->getGridData();
    for(int x = 0; x < trGridGMS.getWidth(); x++) 
    {
	for(int y = 0; y < trGridGMS.getHeight(); y++) 
	{
	    trData[x][y] = trGridGMS.getEntry(x, y);
	}
    }	

    //set correct position of grid in envire
    Affine3d tr;
    tr.setIdentity();
    tr.translation() = trGridGMS.getGridPosition();
    gridPos->setTransform(tr);
}

void ServoingTask::updateSweepingState(Eigen::Affine3d const& transformation)
{
    Vector3d angles = transformation.rotation().eulerAngles(2,1,0);
    dynamixelMin = std::min(dynamixelMin, angles[2]);
    dynamixelMax = std::max(dynamixelMax, angles[2]);

    if ( !justStarted && (!dynamixelMaxFixed || !dynamixelMinFixed) ) {
        int dir;

        if (angles[2] > dynamixelAngle) dir = 1;
        else if ( angles[2] < dynamixelAngle ) dir = -1;
        else dir = 0;

        if ( dynamixelDir - dir == 2 ) dynamixelMaxFixed = true;
        else if ( dynamixelDir - dir == -2 ) dynamixelMinFixed = true;

        if ( dir != 0 ) dynamixelDir = dir;
    }

    dynamixelAngle = angles[2];

    //track sweep status
    switch (sweepStatus)
    {
        case SWEEP_DONE:
        case SWEEP_UNTRACKED:
            break;
        case WAITING_FOR_START:
            if(fabs(dynamixelMax - dynamixelAngle) < 0.05)
            {
                RTT::log(RTT::Info) << "Sweep started" << RTT::endlog(); 
                sweepStatus = SWEEP_STARTED;
            }
            break;
        case SWEEP_STARTED:
            if(fabs(dynamixelMin - dynamixelAngle) < 0.05)
            {
                RTT::log(RTT::Info) << "Sweep done" << RTT::endlog(); 
                sweepStatus = SWEEP_DONE;
            }
            break;
    }
}

inline Eigen::Affine3d XFORWARD2YFORWARD(Eigen::Affine3d const& x2x)
{
    Eigen::Affine3d y2x(Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d::UnitZ()));
    Eigen::Affine3d x2y(Eigen::AngleAxisd(-M_PI / 2, Eigen::Vector3d::UnitZ()));
    return y2x * x2x * x2y;
}

inline Eigen::Affine3d YFORWARD2XFORWARD(Eigen::Affine3d const& y2y)
{
    Eigen::Affine3d y2x(Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d::UnitZ()));
    Eigen::Affine3d x2y(Eigen::AngleAxisd(-M_PI / 2, Eigen::Vector3d::UnitZ()));
    return x2y * y2y * y2x;
}

void ServoingTask::scan_samplesTransformerCallback(const base::Time& ts, const base::samples::LaserScan& scan_reading)
{
    Eigen::Affine3d laser2BodyCenter;
    if(!_laser2body_center.get(ts, laser2BodyCenter, true))
	return;

    if (_x_forward.get())
        laser2BodyCenter = XFORWARD2YFORWARD(laser2BodyCenter);
    updateSweepingState(laser2BodyCenter);

    Eigen::Affine3d body_center_to_odo;
    if(!_body_center2odometry.get(ts, body_center_to_odo, true))
        return;
    bodyCenter2Odo = body_center_to_odo;
    if (_x_forward.get())
        bodyCenter2Odo = XFORWARD2YFORWARD(bodyCenter2Odo);

    if(!_body_center2body.get(ts, bodyCenter2Body, true))
	return;
    if (_x_forward.get())
        bodyCenter2Body = XFORWARD2YFORWARD(bodyCenter2Body);

    mapGenerator->moveMapIfRobotNearBoundary(bodyCenter2Odo.translation());
    
    //note this has to be done after moveMapIfRobotNearBoundary
    //as moveMapIfRobotNearBoundary moves the map to the robot position
    if(justStarted)
    {
        if ( aprioriMap) {
            const Eigen::Affine3d aprioriMap2BodyCenter(bodyCenter2Body.inverse() * aprioriMap2Body);
            const Eigen::Affine3d apriori2LaserGrid(bodyCenter2Odo * aprioriMap2BodyCenter);
            mapGenerator->addKnowMap(aprioriMap.get(), apriori2LaserGrid);

            aprioriMap.reset(0);
            gotNewMap = true;
        }

        justStarted = false;
    } 

    if ( !markedRobotsPlace && dynamixelMaxFixed && dynamixelMinFixed ) {

        TreeSearchConf search_conf(_search_conf.value());
        double val = search_conf.robotWidth + search_conf.obstacleSafetyDistance + search_conf.stepDistance;

        double front_shadow = _front_shadow_distance.get();

        if ( front_shadow <= 0.0 ) {
            double laser_height = laser2BodyCenter.translation().z() + _height_to_ground.get();
            front_shadow = laser_height / tan(-dynamixelMin);
            RTT::log(RTT::Info) << "front shadow distance from tilt: " << front_shadow << RTT::endlog();
        }

        front_shadow += laser2BodyCenter.translation().y() - val / 2.0;
        mapGenerator->markUnknownInRectangeAsTraversable(base::Pose(bodyCenter2Odo), val, val, front_shadow);
        markedRobotsPlace = true;
    }

    // addLaserScan is behind the if, to not add laser scans, when the robot's place is not marked
    if ( markedRobotsPlace )
        gotNewMap |= mapGenerator->addLaserScan(scan_reading, bodyCenter2Odo, laser2BodyCenter);

    base::samples::RigidBodyState laser2Map;
    if (_x_forward.get())
        laser2Map.setTransform(YFORWARD2XFORWARD(mapGenerator->getLaser2Map()));
    else
        laser2Map.setTransform(mapGenerator->getLaser2Map());
    laser2Map.sourceFrame = "laser";
    laser2Map.targetFrame = "map";
    laser2Map.time = ts;
    
    _debug_laser_frame.write(laser2Map);
}

bool ServoingTask::setMap(::std::vector< ::envire::BinaryEvent > const & map, ::std::string const & mapId, ::base::samples::RigidBodyState const & mapPose)
{
    if(!justStarted)
	return false;
    
    envire::Environment env;
    env.applyEvents(map);
    
    aprioriMap = env.getItem< envire::MLSGrid >(mapId);
    if (!aprioriMap)
        return false;
    
    aprioriMap2Body = mapPose.getTransform().inverse();
    return true;
}

/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See ServoingTask.hpp for more detailed
// documentation about them.

bool ServoingTask::configureHook()
{    
    if (!ServoingTaskBase::configureHook())
        return false;

    vfhServoing->setCostConf(_cost_conf.get());
    vfhServoing->setSearchConf(_search_conf.get());
    
    //maximum distance of the horizon in the map
    double boundarySize = _search_horizon.get() + _cost_conf.get().obstacleSenseRadius + _search_conf.get().stepDistance;
    
    //add a third, as a rule of thumb to avoid problems
    boundarySize *= 1.3;
    
    mapGenerator->setBoundarySize(boundarySize);
    mapGenerator->setMaxStepSize(_search_conf.get().maxStepSize);

    failCount = _fail_count.get();
    unknownRetryCount = _unknown_retry_count.get();

    markedRobotsPlace = false;

    justStarted = true;

    return true;
}

bool ServoingTask::startHook()
{  
    if(!ServoingTaskBase::startHook())
	return false;
    
    gotNewMap = false;
    justStarted = true;
    sweepStatus = SWEEP_UNTRACKED;
    noTrCounter = 0;
    unknownTrCounter = 0;

    mapGenerator->setHeightToGround(_height_to_ground.get());
    mapGenerator->clearMap();
    copyGrid();

    vfhServoing->setNewTraversabilityGrid(trGrid);
    
    bodyCenter2Odo = Affine3d::Identity();
    
    dynamixelMin = std::numeric_limits< double >::max();
    dynamixelMax = -std::numeric_limits< double >::max();
    dynamixelMinFixed = false;
    dynamixelMaxFixed = false;
    dynamixelDir = 0;

    return true;
}




void ServoingTask::updateHook()
{
    base::samples::RigidBodyState odometry_reading;
    while( _odometry_samples.read(odometry_reading, false) == RTT::NewData )
	_transformer.pushDynamicTransformation( odometry_reading );	

    ServoingTaskBase::updateHook();

    if (gotNewMap && markedRobotsPlace)
    {
	mapGenerator->computeNewMap();

	TreeSearchConf search_conf(_search_conf.value());
	const base::Pose curPose(bodyCenter2Odo);
	const double obstacleDist = search_conf.robotWidth + search_conf.obstacleSafetyDistance + search_conf.stepDistance + _search_conf.get().stepDistance * 2.0;
	//mark all unknown beside the robot as obstacle, but none in front of the robot
	mapGenerator->markUnknownInRectangeAsObstacle(curPose, obstacleDist, obstacleDist, -_search_conf.get().stepDistance * 2.0);
    }
    
    // Output the map
    if (_gridDump.connected() && gotNewMap)
    {
        vfh_star::GridDump gd;
        mapGenerator->getGridDump(gd);

        if (_x_forward.get())
        {
            vfh_star::GridDump gd_xforward;
            int line_size = GRIDSIZE / GRIDRESOLUTION;
            int array_size = gd_xforward.height.size();
            for (int y = 0; y < line_size; ++y)
                for (int x = 0; x < line_size; ++x)
                {
                    gd_xforward.height[x * line_size + (line_size - y)] = gd.height[y * line_size + x];
                    gd_xforward.max[x * line_size + (line_size - y)] = gd.max[y * line_size + x];
                    gd_xforward.interpolated[x * line_size + (line_size - y)] = gd.interpolated[y * line_size + x];
                    gd_xforward.traversability[x * line_size + (line_size - y)] = gd.traversability[y * line_size + x];
                }
            gd_xforward.gridPositionX = gd.gridPositionY;
            gd_xforward.gridPositionY = -gd.gridPositionX;
            gd_xforward.gridPositionZ = -gd.gridPositionZ;
            _gridDump.write(gd_xforward);
        }
        else
        {
            _gridDump.write(gd);
        }
    }

    if(_heading.readNewest(globalHeading) == RTT::NoData)
    {
	//write empty trajectory to stop robot
	_trajectory.write(std::vector<base::Trajectory>());
        return;
    }
    
    // Plan only if required and if we have a new map
    if(_trajectory.connected() || (gotNewMap || sweepStatus == SWEEP_DONE)) {
	//notify the servoing that there is a new map
	vfhServoing->setNewTraversabilityGrid(trGrid);
	
	vfhServoing->clearDebugData();
	
	base::Pose frontArea(bodyCenter2Odo);
	frontArea.position += frontArea.orientation * Vector3d(0, 0.5, 0);
	vfh_star::ConsistencyStats frontArealStats = mapGenerator->checkMapConsistencyInArea(frontArea, 0.5, 0.5);
	
	//copy data to envire grid
	copyGrid();
	
	//only go onto terrain we know something about
	//or if we can not gather any more information
	if(frontArealStats.averageCertainty > 0.3 || sweepStatus == SWEEP_DONE)
	{
	    base::Time start = base::Time::now();
	    std::vector<base::Trajectory> tr;
	    
	    //set correct Z value according to the map
	    Eigen::Affine3d bodyCenter2Odo_zCorrected(bodyCenter2Odo);
	    mapGenerator->getZCorrection(bodyCenter2Odo_zCorrected);
	    
	    VFHServoing::ServoingStatus status = vfhServoing->getTrajectories(tr, base::Pose(bodyCenter2Odo_zCorrected), globalHeading, _search_horizon.get(), bodyCenter2Body);
	    base::Time end = base::Time::now();

            Eigen::Affine3d y2x(Eigen::AngleAxisd(-M_PI / 2, Eigen::Vector3d::UnitZ()));
            if (_x_forward.get())
            {
                for (int i = 0; i < tr.size(); ++i)
                    tr[i].spline.transform(y2x);
            }
            
	    _trajectory.write(tr);
            RTT::log(RTT::Info) << "vfh took " << (end-start).toMicroseconds() << RTT::endlog(); 

	    if (_vfhDebug.connected())
		_vfhDebug.write(vfhServoing->getVFHStarDebugData(std::vector<base::Waypoint>()));
	    if (_debugVfhTree.connected())
		_debugVfhTree.write(vfhServoing->getTree());
           
	    if(status == VFHServoing::TRAJECTORY_THROUGH_UNKNOWN)
	    {
		if(sweepStatus == SWEEP_DONE)
		    unknownTrCounter++;
		
		if(unknownTrCounter > unknownRetryCount)
		{
                    RTT::log(RTT::Error) << "Quitting, trying to drive through unknown terrain" << RTT::endlog();
		    return exception(TRAJECTORY_THROUGH_UNKNOWN);
		}
	    }
	    else
	    {
		unknownTrCounter = 0;
	    }
		
	    if(status == VFHServoing::NO_SOLUTION)
	    {
	        RTT::log(RTT::Warning) << "Could not compute trajectory towards target horizon" << RTT::endlog();
		
		noTrCounter++;
		if(noTrCounter > failCount)
		    return exception(NO_SOLUTION);
	    } 
	    else
	    {
		noTrCounter = 0;
	    }
	    
	    if(sweepStatus == SWEEP_DONE)
		sweepStatus = SWEEP_UNTRACKED;
	} else {	    
	    //we need to wait a full sweep
	    if(sweepStatus == SWEEP_UNTRACKED)
	    {
                RTT::log(RTT::Info) << "Waiting until sweep is completed" << RTT::endlog(); 
		sweepStatus = WAITING_FOR_START;
	    }
	    //do not write an empty trajectory here
	    //if we reached this code path we allready
	    //waited for a whole sweep and a valid
	    //trajectory was planned.
	}
    }

    gotNewMap = false;	
}

// void ServoingTask::errorHook()
// {
// }
void ServoingTask::stopHook()
{
    justStarted = true;

    //write empty trajectory to stop robot
    _trajectory.write(std::vector<base::Trajectory>());
    ServoingTaskBase::stopHook();
}
// void ServoingTask::cleanupHook()
// {
// }

