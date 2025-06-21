#pragma once

#include <highscore/libhighscore.h>

G_BEGIN_DECLS

#define SAMEBOY_TYPE_CORE (sameboy_core_get_type())

G_DECLARE_FINAL_TYPE (SameBoyCore, sameboy_core, SAMEBOY, CORE, HsCore)

G_MODULE_EXPORT GType hs_get_core_type (void);

G_END_DECLS