extern "C" {
#include <base/byteorder.h>
#include <base/log.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/udp.h>
}

#include <iostream>
#include "sync.h"

static netaddr listen_addr;
static double cycles_per_ns;

static void __attribute__((noinline)) fake_work(unsigned int nloops) {
  for (unsigned int i = 0; i++ < nloops;) {
    asm volatile("nop");
  }
}

// PSP Message format
struct PspMb {
  uint32_t id;
  uint32_t req_type;
  uint32_t reqsize;
  uint32_t run_ns;
};

// Shenango loadgen message format
struct Payload {
  uint64_t work_iterations;
  uint64_t index;
  uint64_t randomness;
};

static void HandleRequest(udp_spawn_data *d) {
  unsigned int niters;
  if (d->len == sizeof(PspMb)) {
    PspMb *p = (PspMb *)d->buf;
    niters = static_cast<double>(p->run_ns) * cycles_per_ns;
  } else if (d->len == sizeof(Payload)) {
    Payload *p = (Payload *)d->buf;
    niters = static_cast<double>(ntoh64(p->work_iterations)) * cycles_per_ns;
  } else {
    panic("invalid message len %lu", d->len);
  }
  fake_work(niters);
  if (udp_respond(d->buf, d->len, d) != static_cast<ssize_t>(d->len))
    panic("bad write");
  udp_spawn_data_release(d->release_data);
}

static void MainHandler(void *arg) {
  udpspawner_t *s;

  cycles_per_ns = static_cast<double>(cycles_per_us) / 1000.0;

  int ret = udp_create_spawner(listen_addr, HandleRequest, &s);
  if (ret) panic("ret %d", ret);

  rt::WaitGroup w(1);
  w.Wait();
}

int main(int argc, char *argv[]) {
  int ret;

  if (argc != 3) {
    std::cerr << "usage: [cfg_file] [portno]" << std::endl;
    return -EINVAL;
  }

  listen_addr.port = atoi(argv[2]);

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    std::cerr << "failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
