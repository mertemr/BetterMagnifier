#pragma once

// Resource identifiers. Kept in a header of its own because BetterMagnifier.rc
// is compiled by rc.exe, which understands #include and #define and very little
// else — it cannot see anything in pch.h.

#ifndef BETTER_MAGNIFIER_RESOURCE_H
#define BETTER_MAGNIFIER_RESOURCE_H

// Explorer shows the icon with the LOWEST numeric id as the file's icon, so the
// application icon has to sort first. Renumbering these is not cosmetic.
#define IDI_APP_ICON    101
#define IDI_TRAY_ON     102
#define IDI_TRAY_OFF    103

#endif // BETTER_MAGNIFIER_RESOURCE_H
