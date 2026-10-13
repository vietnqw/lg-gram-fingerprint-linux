/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "device.h"
#include "error.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>

struct _GvDevice
{
  int spi_fd;
  int reset_fd;   /* GPIO v2 line request, held for the device lifetime */
};

static gboolean
spi_xfer (GvDevice *dev, const guint8 *tx, guint8 *rx, gsize len, GError **error)
{
  struct spi_ioc_transfer t = {
    .tx_buf = (guintptr) tx,
    .rx_buf = (guintptr) rx,
    .len = len,
    .speed_hz = GV_SPI_SPEED_HZ,
    .bits_per_word = 8,
  };

  if (ioctl (dev->spi_fd, SPI_IOC_MESSAGE (1), &t) < 0)
    {
      if (errno == EMSGSIZE)
        return gv_error_false (error, GV_ERROR_IO,
                               "SPI transfer of %zu bytes too large: load spidev with bufsiz=65536", len);
      return gv_error_false (error, GV_ERROR_IO, "SPI transfer failed: %s", g_strerror (errno));
    }
  return TRUE;
}

static gboolean
set_reset (GvDevice *dev, gboolean active, GError **error)
{
  struct gpio_v2_line_values v = { .bits = active ? 1 : 0, .mask = 1 };

  if (ioctl (dev->reset_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0)
    return gv_error_false (error, GV_ERROR_IO, "reset GPIO: %s", g_strerror (errno));
  return TRUE;
}

GvDevice *
gv_device_open (const char *spidev, const char *gpiochip, guint reset_line, GError **error)
{
  g_autoptr(GvDevice) dev = g_new0 (GvDevice, 1);
  guint32 mode = SPI_MODE_0 | SPI_CS_HIGH, speed = GV_SPI_SPEED_HZ;
  guint8 bits = 8;
  struct gpio_v2_line_request req = { 0 };
  int chip;

  dev->spi_fd = dev->reset_fd = -1;

  dev->spi_fd = open (spidev, O_RDWR | O_CLOEXEC);
  if (dev->spi_fd < 0)
    return gv_error_null (error, GV_ERROR_IO, "open %s: %s", spidev, g_strerror (errno));
  if (ioctl (dev->spi_fd, SPI_IOC_WR_MODE32, &mode) < 0 ||
      ioctl (dev->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl (dev->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
    return gv_error_null (error, GV_ERROR_IO, "configure %s: %s", spidev, g_strerror (errno));

  chip = open (gpiochip, O_RDWR | O_CLOEXEC);
  if (chip < 0)
    return gv_error_null (error, GV_ERROR_IO, "open %s: %s", gpiochip, g_strerror (errno));
  req.offsets[0] = reset_line;
  req.num_lines = 1;
  req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
  g_strlcpy (req.consumer, "geneva-fp reset", sizeof req.consumer);
  if (ioctl (chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
    {
      int e = errno;

      close (chip);
      return gv_error_null (error, GV_ERROR_IO, "request reset line %s:%u: %s",
                            gpiochip, reset_line, g_strerror (e));
    }
  close (chip);
  dev->reset_fd = req.fd;
  return g_steal_pointer (&dev);
}

void
gv_device_close (GvDevice *dev)
{
  if (!dev)
    return;
  if (dev->spi_fd >= 0)
    close (dev->spi_fd);
  if (dev->reset_fd >= 0)
    close (dev->reset_fd);
  g_free (dev);
}

gboolean
gv_device_reset (GvDevice *dev, GError **error)
{
  if (!set_reset (dev, TRUE, error))
    return FALSE;
  g_usleep (10 * 1000);
  if (!set_reset (dev, FALSE, error))
    return FALSE;
  g_usleep (420 * 1000);   /* MCU boot + app start */
  return TRUE;
}

gboolean
gv_device_write (GvDevice *dev, const guint8 *data, gsize len, GError **error)
{
  gsize n = len + (len & 1);
  g_autofree guint8 *tx = g_malloc0 (5 + n);

  tx[0] = 0xf0;
  tx[1] = 0x3d;
  tx[2] = 0x00;
  tx[3] = n >> 8;
  tx[4] = n & 0xff;
  memcpy (tx + 5, data, len);
  return spi_xfer (dev, tx, NULL, 5 + n, error);
}

gboolean
gv_device_read (GvDevice *dev, guint8 *buf, gsize len, GError **error)
{
  static const guint8 hdr[3] = { 0xf0, 0x3b, 0x00 };
  g_autofree guint8 *tx = g_malloc0 (len + 1);
  g_autofree guint8 *rx = g_malloc (len + 1);

  if (!spi_xfer (dev, hdr, NULL, sizeof hdr, error))
    return FALSE;
  tx[0] = 0xf1;
  if (!spi_xfer (dev, tx, rx, len + 1, error))
    return FALSE;
  memcpy (buf, rx + 1, len);
  return TRUE;
}
