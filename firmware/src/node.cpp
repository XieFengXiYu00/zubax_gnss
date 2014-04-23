/*
 * Copyright (c) 2014 Courierdrone, courierdrone.com
 * Distributed under the MIT License, available in the file LICENSE.
 * Author: Pavel Kirienko <pavel.kirienko@courierdrone.com>
 */

#include "node.hpp"

#include "board/board.hpp"
#include <ch.hpp>
#include <unistd.h>

#include <crdr_chibios/config/config.hpp>
#include <crdr_chibios/sys/sys.h>

namespace node
{
namespace
{

crdr_chibios::config::Param<unsigned> can_bitrate("can_bitrate", 1000000, 20000, 1000000);
crdr_chibios::config::Param<unsigned> node_id("uavcan_node_id", 42, 1, 120);

uavcan_stm32::CanInitHelper<> can;

uavcan_stm32::Mutex node_mutex;

ComponentStatusManager comp_stat_mgr(uavcan::protocol::NodeStatus::STATUS_INITIALIZING);

void configureNode()
{
    Node& node = getNode();

    node.setNodeID(node_id.get());
    node.setName("com.courierdrone.gps");

    uavcan::protocol::SoftwareVersion swver;
    swver.major = FW_VERSION_MAJOR;
    swver.minor = FW_VERSION_MINOR;
    node.setSoftwareVersion(swver);

    uavcan::protocol::HardwareVersion hwver;
    hwver.major = HW_VERSION;
    node.setHardwareVersion(hwver);
}

/*
 * UAVCAN spin loop
 */
class : public chibios_rt::BaseStaticThread<3000>
{
    uavcan::MonotonicTime prev_led_update;

public:
    msg_t main() override
    {
        configureNode();

        /*
         * Starting the UAVCAN node - this may take a few seconds
         */
        while (true)
        {
            {
                Lock locker;
                const int uavcan_start_res = getNode().start();
                if (uavcan_start_res >= 0)
                {
                    break;
                }
                lowsyslog("Node initialization failure: %i, will try agin soon\n", uavcan_start_res);
            }
            ::sleep(3);
        }
        assert(getNode().isStarted());

        /*
         * Main loop
         */
        lowsyslog("UAVCAN node started\n");
        auto& node = getNode();
        while (true)
        {
            {
                Lock locker;

                node.getNodeStatusProvider().setStatusCode(comp_stat_mgr.getWorstStatusCode());

                const int spin_res = node.spin(uavcan::MonotonicDuration::fromUSec(500));
                if (spin_res < 0)
                {
                    lowsyslog("UAVCAN spin failure: %i\n", spin_res);
                }
            }

            // Iface LED update
            const auto ts = uavcan_stm32::clock::getMonotonic();
            if ((ts - prev_led_update).toMSec() >= 25)
            {
                prev_led_update = ts;
                for (unsigned i = 0; i < can.driver.getNumIfaces(); i++)
                {
                    board::setCANLed(i, can.driver.getIface(i)->hadActivity());
                }
            }

            ::usleep(1000);
        }
        return msg_t();
    }
} node_thread;

}

Lock::Lock() : uavcan_stm32::MutexLocker(node_mutex) { }

bool isStarted()
{
    return getNode().isStarted();
}

Node& getNode()
{
    static Node node(can.driver, uavcan_stm32::SystemClock::instance());
    return node;
}

void setComponentStatus(ComponentID comp, ComponentStatusManager::StatusCode status)
{
    comp_stat_mgr.setComponentStatus(comp, status);
}

int init()
{
    const int can_res = can.init(can_bitrate.get());
    if (can_res < 0)
    {
        return can_res;
    }

    (void)node_thread.start(LOWPRIO);
    return 0;
}

}
