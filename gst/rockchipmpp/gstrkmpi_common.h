#pragma once

#include <glib.h>

gboolean gst_rkmpi_init();
void gst_rkmpi_exit();

#define COUNTOF(x) (sizeof(x) / sizeof((x)[0]))
#define RK_MPI_ERROR_CHECK(name)                                               \
  if (rkret != RK_SUCCESS) {                                                   \
    fprintf(stderr, "rockit MPI: %s failed (%d)\n", #name, rkret);             \
    return GST_FLOW_ERROR;                                                     \
  }
#define RK_MPI_ERROR_CHECKV(name)                                              \
  if (rkret != RK_SUCCESS) {                                                   \
    fprintf(stderr, "rockit MPI: %s failed (%d)\n", #name, rkret);             \
    return;                                                                    \
  }
#define RK_MPI_ERROR_CHECK2(name)                                              \
  if (rkret != RK_SUCCESS) {                                                   \
    fprintf(stderr, "rockit MPI: %s failed (%d)\n", #name, rkret);             \
    return FALSE;                                                              \
  }
#define RK_MPI_ERROR_CHECK_NULL(v, name)                                       \
  if (v) {                                                                     \
    fprintf(stderr, "rockit MPI: %s returned NULL!\n", #name);                 \
    return FALSE;                                                              \
  }
