#pragma once

#include <model_path.h>

namespace meet {

/** Shared esp-sr model list from the `model` partition. Null if missing. */
srmodel_list_t* SrModels();

}  // namespace meet
