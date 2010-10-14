#include <Eigen/StdVector>
#include "Task.hpp"
#include <rtt/NonPeriodicActivity.hpp>
#include <vfh_star/VFHStar.h>
#include <vfh_star/VFH.h>
#include <asguard/Configuration.hpp>

using namespace corridor_servoing;
using namespace vfh_star;
using namespace Eigen;

class VFHServoing: public VFHStar {
    public:
	VFHServoing(const envire::Grid<Traversability> *tr): vfh(tr), vfhDebug(&debugData) {};
	virtual ~VFHServoing() {};
	wrappers::VFHStarDebugData getVFHStarDebugData(const std::vector<base::Waypoint> &trajectory);
    private:
	VFH vfh;
	wrappers::VFHStarDebugData *vfhDebug;
	wrappers::VFHStarDebugData debugData;

	virtual std::vector< std::pair< double, double > > getNextPossibleDirections(const base::Pose& curPose) const;
	virtual base::Pose getProjectedPose(const base::Pose& curPose, const double& heading, const double& distance) const;
};

wrappers::VFHStarDebugData VFHServoing::getVFHStarDebugData(const std::vector< base::Waypoint >& trajectory)
{
    wrappers::VFHStarDebugData dd_out;
    for(std::vector<base::Waypoint>::const_iterator it = trajectory.begin(); it != trajectory.end(); it++)
    {
	bool found = false;
	for(std::vector<wrappers::VFHDebugData>::const_iterator it2 = debugData.steps.begin(); it2 != debugData.steps.end(); it2++) 
	{
	    if(it->position == Eigen::Vector3d(it2->poseb.position))
	    {
		dd_out.steps.push_back(*it2);
		found = true;
		break;
	    }
	    
	}
	if(!found && (it + 1) != trajectory.end() )
	{
	    std::cerr << "BAD debug data is fishy" << std::endl;
	    throw std::runtime_error("Could not build VFHStarDebugData");
	}
    }
    return dd_out;
}


std::vector< std::pair< double, double > > VFHServoing::getNextPossibleDirections(const base::Pose& curPose) const
{
    VFHDebugData dd;
    std::vector< std::pair< double, double > > ret;
    ret = vfh.getNextPossibleDirections(curPose, &dd);
    vfhDebug->steps.push_back(dd);
    return ret;
}


base::Pose VFHServoing::getProjectedPose(const base::Pose& curPose, const double& heading, const double& distance) const
{
    //super omnidirectional robot
    Vector3d p(0, distance, 0);
    
    base::Pose ret;
    ret.orientation = AngleAxisd(heading, Vector3d::UnitZ());
    ret.position = curPose.position + ret.orientation * p;
    
    return ret;
}


RTT::NonPeriodicActivity* Task::getNonPeriodicActivity()
{ return dynamic_cast< RTT::NonPeriodicActivity* >(getActivity().get()); }


Task::Task(std::string const& name)
    : TaskBase(name)
{
    aggr = new aggregator::StreamAligner();    
    asguard::Configuration asguardConf;
    laser2Body = asguardConf.laser2Body;
    body2Odo.setIdentity();
    
    globalHeading = 0;
    lastDrivenDirection = 0;
}


void Task::odometry_callback(base::Time ts, const wrappers::samples::RigidBodyState& odometry_reading)
{
    gotOdometry = true;
    body2Odo = odometry_reading;
}

void Task::scan_callback(base::Time ts, const base::samples::LaserScan& scan_reading)
{
    if(!gotOdometry)
	return;
    
    bool gotNewMap = mapGenerator.addLaserScan(scan_reading, body2Odo, laser2Body);

    if(gotNewMap) {
	const TraversabilityGrid &trGridGMS(mapGenerator.getTraversabilityMap());

	//convert to envire::Grid<Traversability>
	envire::Environment env;
	envire::FrameNode gridPos;
	Transform3d tr;
	tr.setIdentity();
	tr.translation() = trGridGMS.getGridPosition();
	gridPos.setTransform(tr);
	env.attachItem(&gridPos);
	envire::Grid<Traversability> trGrid(trGridGMS.getWidth(), trGridGMS.getHeight(), trGridGMS.getGridEntrySize(), trGridGMS.getGridEntrySize());
	envire::Grid<Traversability>::ArrayType &trData = trGrid.getGridData();
	env.attachItem(&trGrid);
	trGrid.setFrameNode(&gridPos);
	for(int x = 0; x < trGridGMS.getWidth(); x++) {
	    for(int y = 0; y < trGridGMS.getHeight(); y++) {
		trData[x][y] = trGridGMS.getEntry(x, y);
	    }			
	}		    

	VFHServoing vfh(&trGrid);

	base::Time start = base::Time::now();

	std::vector<base::Waypoint> trajectory = vfh.getTrajectory(base::Pose(body2Odo), globalHeading, lastDrivenDirection);
	base::Time end = base::Time::now();

	if(trajectory.size())
	{
	    lastDrivenDirection = trajectory.begin()->heading;
	}

	wrappers::Trajectory tr_out;
	for(std::vector<base::Waypoint>::const_iterator it = trajectory.begin(); it != trajectory.end(); it++)
	{
	    tr_out.tr.push_back(*it);
	}
	_trajectory.write(tr_out);

	std::cout << "vfh took " << (end-start).toMicroseconds() << std::endl; 

	//detach items, to avoid invalid free when env get's destroyed
	env.detachItem(&trGrid);
	env.detachItem(&gridPos);
	
	//write out debug output
	vfh_star::GridDump gd;
	mapGenerator.getGridDump(gd);
	_gridDump.write(gd);

 	_vfhDebug.write(vfh.getVFHStarDebugData(trajectory));
    }
}



/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See Task.hpp for more detailed
// documentation about them.

bool Task::configureHook()
{    
    // setup the aggregator with the timeout value provided by the module
    aggr->setTimeout( base::Time::fromSeconds( _max_delay.value() ) );

    const double buffer_size_factor = 2.0;

    od_idx = aggr->registerStream<wrappers::samples::RigidBodyState>(
	   boost::bind( &Task::odometry_callback, this, _1, _2 ),
	   buffer_size_factor* ceil( _max_delay.value()/_odometry_period.value() ),
	   base::Time::fromSeconds( _odometry_period.value() ) );
    
    scan_idx = aggr->registerStream<base::samples::LaserScan>(
	   boost::bind( &Task::scan_callback, this, _1, _2 ),
	   buffer_size_factor* ceil( _max_delay.value()/_scan_period.value() ),
	   base::Time::fromSeconds( _scan_period.value() ) );

    gotOdometry = false;
    
    return true;
}

// bool Task::startHook()
// {
//     return true;
// }




void Task::updateHook(std::vector<RTT::PortInterface*> const& updated_ports)
{
    base::samples::LaserScan scan_reading;
    while( _scan_samples.read(scan_reading) )
    {
	aggr->push( scan_idx, scan_reading.time, scan_reading );	
    }
    
    wrappers::samples::RigidBodyState odometry_reading;
    while( _odometry_samples.read(odometry_reading) )
    {
	aggr->push( od_idx, odometry_reading.time, odometry_reading );	
    }

    // then call the streams in the relevant order
    while( aggr->step() );    
}

// void Task::errorHook()
// {
// }
// void Task::stopHook()
// {
// }
// void Task::cleanupHook()
// {
// }

