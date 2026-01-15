#pragma once

#include <string>
namespace Melee
{
enum class Version
{
  NTSC,
  TwentyXX,
  UPTM,
  MEX,
  OTHER,
};
}

namespace Slippi
{
enum class Chat
{
  CHAT_ON,
  DIRECT_ONLY,
  CHAT_OFF
};

struct Config
{
  Melee::Version melee_version;
  bool oc_enable = true;
  float oc_factor = 1.0f;
  std::string slippi_input = "";
};
}  // namespace Slippi
