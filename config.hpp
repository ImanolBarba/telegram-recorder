#ifndef CONFIG_HPP
#define CONFIG_HPP

#define DEFAULT_CONFIG_FILE "tgrec.conf"

typedef struct HumanBehaviourParams {
  double readMsgFrequencyMean;
  double readMsgFrequencyStdDev;
  double readMsgMinWaitSec;
  double textReadSpeedWPM;
  double photoReadSpeedSec;
} HumanBehaviourParams;

typedef struct ConfigParams {
  int apiID;
  std::string apiHash;
  std::string firstName;
  std::string lastName;
  HumanBehaviourParams humanParams;
} ConfigParams;

#endif