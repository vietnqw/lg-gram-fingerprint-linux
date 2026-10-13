/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "hw.h"
#include "error.h"
#include "proto.h"

#include <string.h>

char *
gv_find_spidev (void)
{
  g_autoptr(GDir) dir = g_dir_open ("/sys/bus/spi/devices", 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *sub = NULL;
      g_autoptr(GDir) d = NULL;
      const char *node;

      if (!strstr (name, GV_ACPI_ID))
        continue;
      sub = g_build_filename ("/sys/bus/spi/devices", name, "spidev", NULL);
      d = g_dir_open (sub, 0, NULL);
      if (d && (node = g_dir_read_name (d)))
        return g_build_filename ("/dev", node, NULL);
    }
  return NULL;
}

gboolean
gv_config_load (const char *path, guint8 *config, GError **error)
{
  g_autofree char *data = NULL;
  gsize len = 0;
  g_autoptr(GError) e = NULL;

  if (!g_file_get_contents (path, &data, &len, &e))
    return gv_error_false (error, GV_ERROR_NO_CONFIG,
                           "sensor configuration missing (%s); run `sudo lg-gram-fp-tool extract <LG driver>`",
                           e->message);
  if (len != GV_CFG_SIZE)
    return gv_error_false (error, GV_ERROR_NO_CONFIG, "%s: expected %d bytes, got %zu",
                           path, GV_CFG_SIZE, len);
  memcpy (config, data, GV_CFG_SIZE);
  if (gv_cfg_checksum (config) != (config[254] | (config[255] << 8)))
    return gv_error_false (error, GV_ERROR_NO_CONFIG, "%s: bad checksum", path);
  return TRUE;
}
