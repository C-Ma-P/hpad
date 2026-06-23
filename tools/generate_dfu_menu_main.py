#!/usr/bin/env python3
"""Generate main.c with the explicit Options -> Wireless Flash DFU flow."""
from pathlib import Path
import sys

source_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
source = source_path.read_text()


def replace_once(old: str, new: str) -> None:
    global source
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"Expected one match, found {count}: {old[:80]!r}")
    source = source.replace(old, new, 1)


replace_once(
    "#include <zephyr/sys/poweroff.h>\n",
    "#include <zephyr/sys/poweroff.h>\n#include <zephyr/sys/reboot.h>\n",
)
replace_once("#define OPTIONS_MENU_MAX_ITEM_COUNT 6U\n", "#define OPTIONS_MENU_MAX_ITEM_COUNT 7U\n")
replace_once(
    "\tOPTIONS_MENU_ACTION_DEVICE_INFO = 3,\n"
    "\tOPTIONS_MENU_ACTION_POWER_OFF = 4,\n"
    "\tOPTIONS_MENU_ACTION_CLOSE = 5,\n",
    "\tOPTIONS_MENU_ACTION_DEVICE_INFO = 3,\n"
    "\tOPTIONS_MENU_ACTION_WIRELESS_FLASH = 4,\n"
    "\tOPTIONS_MENU_ACTION_POWER_OFF = 5,\n"
    "\tOPTIONS_MENU_ACTION_CLOSE = 6,\n",
)

old_item = "\t{ .action = OPTIONS_MENU_ACTION_DEVICE_INFO },\n\t{ .action = OPTIONS_MENU_ACTION_POWER_OFF },\n"
if source.count(old_item) != 2:
    raise RuntimeError("Expected Wireless Flash insertion point in both mode menus")
source = source.replace(
    old_item,
    "\t{ .action = OPTIONS_MENU_ACTION_DEVICE_INFO },\n"
    "\t{ .action = OPTIONS_MENU_ACTION_WIRELESS_FLASH },\n"
    "\t{ .action = OPTIONS_MENU_ACTION_POWER_OFF },\n",
)

replace_once(
    "\tcase OPTIONS_MENU_ACTION_POWER_OFF:\n\t\treturn \"Power Off\";\n",
    "\tcase OPTIONS_MENU_ACTION_WIRELESS_FLASH:\n\t\treturn \"Wireless Flash\";\n"
    "\tcase OPTIONS_MENU_ACTION_POWER_OFF:\n\t\treturn \"Power Off\";\n",
)

replace_once(
    "\t\t\t       const struct modal_menu_state *feedback_menu,\n"
    "\t\t\t       const struct modal_menu_state *power_confirm)\n",
    "\t\t\t       const struct modal_menu_state *feedback_menu,\n"
    "\t\t\t       const struct modal_menu_state *dfu_confirm,\n"
    "\t\t\t       const struct modal_menu_state *power_confirm)\n",
)
replace_once(
    "\t\tfeedback_menu->open || power_confirm->open;\n",
    "\t\tfeedback_menu->open || dfu_confirm->open || power_confirm->open;\n",
)

replace_once(
    "static void enforce_battery_policy(bool usb_power_present,\n",
    "static void perform_wireless_flash(enum macropad_operating_mode mode,\n"
    "\t\t\t\t  bool display_available,\n"
    "\t\t\t\t  bool *display_sleeping)\n"
    "{\n"
    "\tint rc;\n\n"
    "\tshow_transient_message(display_available, display_sleeping,\n"
    "\t\t\"Starting DFU\", \"BLE update mode\", 250);\n\n"
    "\trc = stop_transport(mode);\n"
    "\tif (rc != 0) {\n"
    "\t\tLOG_WRN(\"Failed to stop active transport before DFU: %d\", rc);\n"
    "\t}\n\n"
    "\tstatus_buzzer_set(false);\n"
    "\tstatus_led_set(false);\n"
    "\trc = key_leds_set_all(0U, 0U, 0U);\n"
    "\tif ((rc != 0) && (rc != -ENOTSUP)) {\n"
    "\t\tLOG_WRN(\"Failed to clear LEDs before DFU: %d\", rc);\n"
    "\t}\n\n"
    "\t/* Adafruit nRF52 UF2 bootloader BLE OTA entry request. */\n"
    "\tNRF_POWER->GPREGRET = 0xA8U;\n"
    "\tsys_reboot(SYS_REBOOT_COLD);\n"
    "}\n\n"
    "static void enforce_battery_policy(bool usb_power_present,\n",
)

replace_once(
    "\tstruct modal_menu_state power_confirm = {\n"
    "\t\t.open = false,\n"
    "\t\t.selected_index = 0U,\n"
    "\t\t.first_visible_index = 0U,\n"
    "\t};\n",
    "\tstruct modal_menu_state dfu_confirm = {\n"
    "\t\t.open = false,\n"
    "\t\t.selected_index = 0U,\n"
    "\t\t.first_visible_index = 0U,\n"
    "\t};\n"
    "\tstruct modal_menu_state power_confirm = {\n"
    "\t\t.open = false,\n"
    "\t\t.selected_index = 0U,\n"
    "\t\t.first_visible_index = 0U,\n"
    "\t};\n",
)

replace_once(
    "\t\t\tif (power_confirm.open) {\n"
    "\t\t\t\tmodal_menu_close(&power_confirm);\n"
    "\t\t\t\toptions_menu_close(&options_menu);\n"
    "\t\t\t} else if (feedback_menu.open) {\n",
    "\t\t\tif (dfu_confirm.open) {\n"
    "\t\t\t\tmodal_menu_close(&dfu_confirm);\n"
    "\t\t\t\toptions_menu_close(&options_menu);\n"
    "\t\t\t} else if (power_confirm.open) {\n"
    "\t\t\t\tmodal_menu_close(&power_confirm);\n"
    "\t\t\t\toptions_menu_close(&options_menu);\n"
    "\t\t\t} else if (feedback_menu.open) {\n",
)

replace_once(
    "\t\t\tif (power_confirm.open) {\n"
    "\t\t\t\tmodal_menu_move(&power_confirm, POWER_CONFIRM_ITEM_COUNT, 0);\n"
    "\t\t\t\trc = status_display_render_menu(\"Power Off?\", power_confirm_labels,\n",
    "\t\t\tif (dfu_confirm.open) {\n"
    "\t\t\t\tmodal_menu_move(&dfu_confirm, POWER_CONFIRM_ITEM_COUNT, 0);\n"
    "\t\t\t\trc = status_display_render_menu(\"Wireless Flash?\", power_confirm_labels,\n"
    "\t\t\t\t\tPOWER_CONFIRM_ITEM_COUNT, dfu_confirm.selected_index,\n"
    "\t\t\t\t\tdfu_confirm.first_visible_index);\n"
    "\t\t\t} else if (power_confirm.open) {\n"
    "\t\t\t\tmodal_menu_move(&power_confirm, POWER_CONFIRM_ITEM_COUNT, 0);\n"
    "\t\t\t\trc = status_display_render_menu(\"Power Off?\", power_confirm_labels,\n",
)

replace_once(
    "\t\t\tif (power_confirm.open) {\n"
    "\t\t\t\tmodal_menu_move(&power_confirm, POWER_CONFIRM_ITEM_COUNT,\n"
    "\t\t\t\t\tencoder_delta);\n",
    "\t\t\tif (dfu_confirm.open) {\n"
    "\t\t\t\tmodal_menu_move(&dfu_confirm, POWER_CONFIRM_ITEM_COUNT,\n"
    "\t\t\t\t\tencoder_delta);\n"
    "\t\t\t\tredraw = true;\n"
    "\t\t\t\tcontinue;\n"
    "\t\t\t}\n\n"
    "\t\t\tif (power_confirm.open) {\n"
    "\t\t\t\tmodal_menu_move(&power_confirm, POWER_CONFIRM_ITEM_COUNT,\n"
    "\t\t\t\t\tencoder_delta);\n",
)

replace_once(
    "\t\t\tif (power_confirm.open) {\n"
    "\t\t\t\tbool confirmed = (power_confirm.selected_index == 1U);\n",
    "\t\t\tif (dfu_confirm.open) {\n"
    "\t\t\t\tbool confirmed = (dfu_confirm.selected_index == 1U);\n\n"
    "\t\t\t\tmodal_menu_close(&dfu_confirm);\n"
    "\t\t\t\tif (confirmed) {\n"
    "\t\t\t\t\toptions_menu_close(&options_menu);\n"
    "\t\t\t\t\tperform_wireless_flash(ui_state.operating_mode,\n"
    "\t\t\t\t\t\tdisplay_available, &display_sleeping);\n"
    "\t\t\t\t}\n"
    "\t\t\t\tredraw = true;\n"
    "\t\t\t\tcontinue;\n"
    "\t\t\t}\n\n"
    "\t\t\tif (power_confirm.open) {\n"
    "\t\t\t\tbool confirmed = (power_confirm.selected_index == 1U);\n",
)

replace_once(
    "\t\t\t\t} else if (selected->action == OPTIONS_MENU_ACTION_POWER_OFF) {\n"
    "\t\t\t\t\tmodal_menu_open(&power_confirm, 0U);\n",
    "\t\t\t\t} else if (selected->action == OPTIONS_MENU_ACTION_WIRELESS_FLASH) {\n"
    "\t\t\t\t\tmodal_menu_open(&dfu_confirm, 0U);\n"
    "\t\t\t\t} else if (selected->action == OPTIONS_MENU_ACTION_POWER_OFF) {\n"
    "\t\t\t\t\tmodal_menu_open(&power_confirm, 0U);\n",
)

replace_once(
    "\t\t\tbool overlay_open = local_overlay_open(&options_menu, &device_info,\n"
    "\t\t\t\t&feedback_menu, &power_confirm);\n",
    "\t\t\tbool overlay_open = local_overlay_open(&options_menu, &device_info,\n"
    "\t\t\t\t&feedback_menu, &dfu_confirm, &power_confirm);\n",
)

out_path.parent.mkdir(parents=True, exist_ok=True)
out_path.write_text(source)
