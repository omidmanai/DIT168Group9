#include "V2VService.hpp"

std::shared_ptr<cluon::OD4Session> od4;
std::queue <float> delayQueue;
bool cantMove = false;

int main(int argc, char **argv) {
    int returnValue = 0;
    auto commandlineArguments = cluon::getCommandlineArguments(argc, argv);
    if (0 == commandlineArguments.count("cid") || 0 == commandlineArguments.count("freq") ||
        0 == commandlineArguments.count("ip") || 0 == commandlineArguments.count("id") ||
        0 == commandlineArguments.count("partnerIp") || 0 == commandlineArguments.count("partnerId") ||
        0 == commandlineArguments.count("speed") || 0 == commandlineArguments.count("left") ||
        0 == commandlineArguments.count("right") || 0 == commandlineArguments.count("queueMax")) {

        std::cerr << argv[0] << " sends and receives follower or leader status as defined in the V2V Protocol (DIT168)."
                  << std::endl;
        std::cerr << "Usage: " << argv[0]
                  << "--cid=<OD4Session Session> --freq=<frequency> --ip=<localIP> --id=<DIT168Group>"
                     " --partnerIp=<Partnering vehicle IP> --partnerId<Partnering vehicle ID> --speed<Speed offset>"
                     " --left<Left angle offset> --right<Right angle offset> --queueMax<Delay for following>"
                  << std::endl;
        std::cerr << "Example: " << argv[0]
                  << "--cid=111 --freq=125 --ip=127.0.0.1 --id=9 --partnerIp=1.1.1.1 --partnerId=8 --speed=0.02"
                     " --left=0.35 --right=-0.05 --queueMax=15"
                  << std::endl;
        returnValue = 1;
    }
    else {
        const uint16_t CID = (uint16_t) std::stoi(commandlineArguments["cid"]);
        const uint16_t FREQ = (uint16_t) std::stoi(commandlineArguments["freq"]);
        const uint16_t QUEUE_MAX = (uint16_t) std::stoi(commandlineArguments["queueMax"]);
        const std::string IP = commandlineArguments["ip"];
        const std::string ID = commandlineArguments["id"];
        const std::string PARTNER_IP = commandlineArguments["partnerIp"];
        const std::string PARTNER_ID = commandlineArguments["partnerId"];
        const std::string SPEED_AFTER = commandlineArguments["speed"];
        const std::string LEFT = commandlineArguments["left"];
        const std::string RIGHT = commandlineArguments["right"];

        std::shared_ptr<V2VService> v2vService = std::make_shared<V2VService>(IP, ID, PARTNER_IP, PARTNER_ID,
                                                                              SPEED_AFTER, LEFT, RIGHT, QUEUE_MAX);

        float pedalPos = 0, steeringAngle = 0, distanceReading = 0;
        uint16_t button = 0;

        // OD4Session for sending and receiving messages internally
        od4 =
        std::make_shared<cluon::OD4Session>(CID,
        [&v2vService, &pedalPos, &steeringAngle, &button, &PARTNER_IP, &distanceReading](cluon::data::Envelope &&envelope) noexcept {
            if (envelope.dataType() == 1041) {
                opendlv::proxy::PedalPositionReading ppr =
                        cluon::extractMessage<opendlv::proxy::PedalPositionReading>(std::move(envelope));
                pedalPos = ppr.position();
            }
            else if (envelope.dataType() == 1043) {
                opendlv::proxy::ButtonPressed buttonPressed =
                        cluon::extractMessage<opendlv::proxy::ButtonPressed>(std::move(envelope));
                button = buttonPressed.buttonNumber();
                switch (button) {
                    case 0: // Square
                        v2vService->followRequest();
                        break;
                    case 1: { // X
                        opendlv::proxy::PedalPositionReading msgPedal;
                        opendlv::proxy::GroundSteeringReading msgSteering;

                        msgPedal.position(0);
                        od4->send(msgPedal);

                        msgSteering.groundSteering(0);
                        od4->send(msgSteering);
                    }
                        break;
                    case 2: // Circle
                        v2vService->stopFollow(PARTNER_IP);
                        break;
                    case 3: // Triangle
                        v2vService->announcePresence();
                        break;
                    default:
                        std::cout << "V2V does not use this button" << std::endl;
                        break;
                }
            }
            else if (envelope.dataType() == 1045) {
                opendlv::proxy::GroundSteeringReading gsr =
                        cluon::extractMessage<opendlv::proxy::GroundSteeringReading>(std::move(envelope));
                steeringAngle = gsr.groundSteering();
            }
            else if (envelope.dataType() == 1039) {
                opendlv::proxy::DistanceReading dist =
                        cluon::extractMessage<opendlv::proxy::DistanceReading>(std::move(envelope));
                distanceReading = dist.distance();

                // Emergency break check using ultrasonic reading
                if (distanceReading < 0.25f) {
                    opendlv::proxy::PedalPositionReading msgPedal;
                    opendlv::proxy::GroundSteeringReading msgSteering;

                    msgPedal.position(0);
                    od4->send(msgPedal);

                    msgSteering.groundSteering(0);
                    od4->send(msgSteering);

                    cantMove = true;
                }
                else {
                    cantMove = false;
                }
            }
        });

        int count = 0;

        // Sending leaderStatus at 125ms and followerStatus at 500ms (once every 4 leaderStatus sent)
        auto atFrequency{[&v2vService, &pedalPos, &steeringAngle, &count]() -> bool {
            if (count == 3) {
                v2vService->followerStatus();
                count = 0;
            }
            v2vService->leaderStatus(pedalPos, steeringAngle, 0);
            count++;
            return true;
        }};
        od4->timeTrigger(FREQ, atFrequency);
    }
}

/**
 * Implementation of the V2VService class as declared in V2VService.hpp
 */
V2VService::V2VService(std::string ip, std::string id, std::string partnerIp, std::string partnerId,
                       std::string speed_after, std::string left, std::string right, uint16_t queue_max) {
    _IP = ip;
    _ID = id;
    _PARTNER_IP = partnerIp;
    _PARTNER_ID = partnerId;
    _SPEED_AFTER = speed_after;
    _LEFT = left;
    _RIGHT = right;
    _QUEUE_MAX = queue_max;

    /*
     * The broadcast field contains a reference to the broadcast channel which is an OD4Session. This is where
     * AnnouncePresence messages will be received.
     */
    broadcast =
    std::make_shared<cluon::OD4Session>(BROADCAST_CHANNEL,
    [this](cluon::data::Envelope &&envelope) noexcept {
        std::cout << "[OD4] ";
        switch (envelope.dataType()) {
            case ANNOUNCE_PRESENCE: {
                AnnouncePresence ap = cluon::extractMessage<AnnouncePresence>(std::move(envelope));
                std::cout << "received 'AnnouncePresence' from '"
                          << ap.vehicleIp() << "', GroupID '"
                          << ap.groupId() << "'!" << std::endl;
                if (ap.vehicleIp() == _PARTNER_IP && ap.groupId() == _PARTNER_ID) {
                    isPresentPartner = true;
                }
                presentCars[ap.groupId()] = ap.vehicleIp();

                break;
            }
            default: std::cout << "[BROADCAST CHANNEL] Not announce presence." << std::endl;
                break;
        }
    });

    /*
     * Each car declares an incoming UDPReceiver for messages directed at them specifically. This is where messages
     * such as FollowRequest, FollowResponse, StopFollow, etc. are received.
     */
    incoming =
    std::make_shared<cluon::UDPReceiver>("0.0.0.0", DEFAULT_PORT,
    [this](std::string &&data, std::string &&sender, std::chrono::system_clock::time_point &&ts) noexcept {
         std::cout << "[UDP] ";
         std::pair<int16_t, std::string> msg = extract(data);

         switch (msg.first) {
             case FOLLOW_REQUEST: {
                 FollowRequest followRequest = decode<FollowRequest>(msg.second);
                 std::cout << "received '" << followRequest.LongName()
                           << "' from '" << sender << "'!" << std::endl;

                 // After receiving a FollowRequest, check first if the car is already a leader.
                 if (!isLeader) {
                     // If not, add the requester to known follower slot and establish a sending channel.
                     toFollower = std::make_shared<cluon::UDPSender>(_PARTNER_IP, DEFAULT_PORT);
                     followResponse();
                 }
                 break;
             }
             case FOLLOW_RESPONSE: {
                 FollowResponse followResponse = decode<FollowResponse>(msg.second);
                 std::cout << "received '" << followResponse.LongName()
                           << "' from '" << sender << "'!" << std::endl;
                 isFollower = true;
                 break;
             }
             case STOP_FOLLOW: {
                 StopFollow stopFollow = decode<StopFollow>(msg.second);
                 std::cout << "received '" << stopFollow.LongName()
                           << "' from '" << sender << "'!" << std::endl;

                 // Clear either follower or leader slot, depending on current role.
                 unsigned long len = sender.find(':');
                 if (isLeader) {
                     isLeader = false;
                     toFollower.reset();
                 }
                 else if (isFollower) {
                     isFollower = false;
                     toLeader.reset();
                 }
                 break;
             }
             case FOLLOWER_STATUS: {
                 FollowerStatus followerStatus = decode<FollowerStatus>(msg.second);
                 std::cout << "received '" << followerStatus.LongName()
                           << "' from '" << sender << "'!" << std::endl;

                 break;
             }
             case LEADER_STATUS: {
                 if (cantMove) {
                     std::cout << "Car cannot move due to an emergency break" << std::endl;
                     return;
                 }
                 opendlv::proxy::PedalPositionReading msgPedal;
                 opendlv::proxy::GroundSteeringReading msgSteering;

                 LeaderStatus leaderStatus = decode<LeaderStatus>(msg.second);
                 std::cout << "received '" << leaderStatus.LongName()
                           << "' from '" << sender << "'!" << std::endl;
                 std::cout << "LeaderStatus Values, pedalPos: " << leaderStatus.speed() << " steeringAngle: "
                           << leaderStatus.steeringAngle() << std::endl;

                 // Calibrate speed received, then apply it.
                 float floatSpeedAfter = std::stof(_SPEED_AFTER);
                 if (leaderStatus.speed() < floatSpeedAfter) {
                     msgPedal.position(leaderStatus.speed());
                 }
                 else {
                     msgPedal.position(leaderStatus.speed() + floatSpeedAfter);
                 }
                 od4->send(msgPedal);

                 float floatLeft = std::stof(_LEFT);
                 float floatRight = std::stof(_RIGHT);
                 float calibratedAngle = 0.0f;

                 // Calibrate angle received.
                 if (leaderStatus.steeringAngle() == 0) {
                     calibratedAngle = leaderStatus.steeringAngle() + m_OFFSET;
                 }
                 else if (leaderStatus.steeringAngle() > 0) {
                     std::cout << "Left Value: " << (leaderStatus.steeringAngle() + floatLeft)
                             << std::endl;
                     calibratedAngle = (leaderStatus.steeringAngle() + floatLeft);
                 }
                 else if (leaderStatus.steeringAngle() < 0) {
                     std::cout << "Right Value: " << (leaderStatus.steeringAngle() + floatRight)
                               << std::endl;
                     calibratedAngle = leaderStatus.steeringAngle() + floatRight;
                 }

                 // Add angle to queue, and apply first angle in queue if queue max is reached.
                 if (leaderStatus.speed() != 0 && delayQueue.size() < _QUEUE_MAX) {
                     delayQueue.push(calibratedAngle);
                 }
                 else if (leaderStatus.speed() != 0 && delayQueue.size() >= _QUEUE_MAX) {
                     delayQueue.push(calibratedAngle);

                     float delayedSteeringAngle = delayQueue.front();
                     msgSteering.groundSteering(delayedSteeringAngle);
                     od4->send(msgSteering);
                     delayQueue.pop();
                 }

                 break;
             }
             default: std::cout << "¯\\_(ツ)_/¯" << std::endl;
                break;
         }
    });
}

/**
 * This function sends an AnnouncePresence (id = 1001) message on the broadcast channel. It will contain information
 * about the sending vehicle, including the IP and the group identifier.
 */
void V2VService::announcePresence() {
    if (isFollower) {
        std::cout << "Announce presence not sent. Already following a car" << std::endl;
        return;
    }

    AnnouncePresence announcePresence;
    announcePresence.vehicleIp(_IP);
    announcePresence.groupId(_ID);
    od4->send(announcePresence);
    broadcast->send(announcePresence);
}

/**
 * This function sends a FollowRequest (id = 1002) message to a partner vehicle's IP address.
 */
void V2VService::followRequest() {
    if (!isPresentPartner || isFollower) {
        std::cout << "Follow request not sent. isFollower: " << isFollower << " isPresentPartner: " << isPresentPartner
                  << std::endl;
        return;
    }
    toLeader = std::make_shared<cluon::UDPSender>(_PARTNER_IP, DEFAULT_PORT);
    FollowRequest followRequest;
    followRequest.temporaryValue("Follow request message");
    od4->send(followRequest);
    toLeader->send(encode(followRequest));
}

/**
 * This function send a FollowResponse (id = 1003) message and is sent in response to a FollowRequest (id = 1002).
 * This message will contain the NTP server IP for time synchronization between the target and the sender.
 */
void V2VService::followResponse() {
    if (isFollower || isLeader) {
        std::cout << "Follow response not sent. isFollower: " << isFollower << " isLeader: " << isLeader << std::endl;
        return;
    }

    FollowResponse followResponse;
    followResponse.temporaryValue("Follow response message");
    isLeader = true;
    od4->send(followResponse);
    toFollower->send(encode(followResponse));
}

/**
 * This function sends a StopFollow (id = 1004) request on the ip address of the parameter vehicleIp. If the IP address is neither
 * that of the follower nor the leader, this function ends without sending the request message.
 *
 * @param vehicleIp - IP of the target for the request
 */
void V2VService::stopFollow(std::string vehicleIp) {
    StopFollow stopFollow;
    stopFollow.temporaryValue("Stop follow message");

    std::cout << "Inside stop follow, vehicleIp == _PARTNER_IP: " << (vehicleIp == _PARTNER_IP) << " isFollower: "
              << isFollower << " isLeader: " << isLeader << std::endl;

    if (vehicleIp == _PARTNER_IP && isFollower) {
        isFollower = false;
        std::cout << "Car was follower\n";
        toLeader->send(encode(stopFollow));
        od4->send(stopFollow);
        toLeader.reset();
    }
    if (vehicleIp == _PARTNER_IP && isLeader) {
        isLeader = false;
        std::cout << "Car was leader\n";
        toFollower->send(encode(stopFollow));
        od4->send(stopFollow);
        toFollower.reset();
    }
}

/**
 * This function sends a FollowerStatus (id = 3001) message on the leader channel.
 */
void V2VService::followerStatus() {
    if (!isFollower) return;
    FollowerStatus followerStatus;
    followerStatus.temporaryValue("Follower status message");
    od4->send(followerStatus);
    toLeader->send(encode(followerStatus));
}

/**
 * This function sends a LeaderStatus (id = 2001) message on the follower channel.
 *
 * @param speed - current velocity
 * @param steeringAngle - currentFREQ steering angle
 * @param distanceTraveled - distance traveled since last reading
 */
void V2VService::leaderStatus(float speed, float steeringAngle, uint8_t distanceTraveled) {
    if (!isLeader) return;
    LeaderStatus leaderStatus;
    leaderStatus.timestamp(getTime());
    leaderStatus.speed(speed);
    leaderStatus.steeringAngle(steeringAngle - m_OFFSET);
    leaderStatus.distanceTraveled(distanceTraveled);
    od4->send(leaderStatus);
    toFollower->send(encode(leaderStatus));
}

/**
 * Gets the current time.
 *
 * @return current time in milliseconds
 */
uint32_t V2VService::getTime() {
    timeval now;
    gettimeofday(&now, nullptr);
    return (uint32_t ) now.tv_usec / 1000;
}

/**
 * The extraction function is used to extract the message ID and message data into a pair.
 *
 * @param data - message data to extract header and data from
 * @return pair consisting of the message ID (extracted from the header) and the message data
 */
std::pair<int16_t, std::string> V2VService::extract(std::string data) {
    if (data.length() < 10) return std::pair<int16_t, std::string>(-1, "");
    int id, len;
    std::stringstream ssId(data.substr(0, 4));
    std::stringstream ssLen(data.substr(4, 10));
    ssId >> std::hex >> id;
    ssLen >> std::hex >> len;
    return std::pair<int16_t, std::string> (
            data.length() -10 == len ? id : -1,
            data.substr(10, data.length() -10)
    );
};

/**
 * Generic encode function used to encode a message before it is sent.
 *
 * @tparam T - generic message type
 * @param msg - message to encode
 * @return encoded message
 */
template <class T>
std::string V2VService::encode(T msg) {
    cluon::ToProtoVisitor v;
    msg.accept(v);
    std::stringstream buff;
    buff << std::hex << std::setfill('0')
         << std::setw(4) << msg.ID()
         << std::setw(6) << v.encodedData().length()
         << v.encodedData();
    return buff.str();
}

/**
 * Generic decode function used to decode an incoming message.
 *
 * @tparam T - generic message type
 * @param data - encoded message data
 * @return decoded message
 */
template <class T>
T V2VService::decode(std::string data) {
    std::stringstream buff(data);
    cluon::FromProtoVisitor v;
    v.decodeFrom(buff);
    T tmp = T();
    tmp.accept(v);
    return tmp;
}
