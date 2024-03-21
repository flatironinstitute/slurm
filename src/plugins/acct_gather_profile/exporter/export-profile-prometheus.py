#!/bin/env python3

import os
import argparse
import pathlib
import subprocess
import prometheus_client
from http.server import HTTPServer

class Collector(object):
    def __init__(self, dir: str):
        self.dir = dir

    def get_metric(self, name: str):
        try:
            return self.metrics[name]
        except KeyError:
            m = prometheus_client.metrics_core.Metric(f'slurm_profile_{name}',
                                                      f"Sampled slurm acct_gather_profile_exporter {name}",
                                                      'gauge')
            self.metrics[name] = m
            return m

    def collect(self):
        self.metrics = dict()
        try:
            jds = os.scandir(self.dir)
        except OSError:
            return []

        for jd in jds:
            if not jd.is_dir(): continue
            job, step = jd.name.split('.', 1)
            jobid = int(job)
            td = {}
            for me in os.scandir(jd):
                if not me.is_file(): continue
                md = {}
                with open(me, 'r') as mf:
                    for ml in mf:
                        k, v = ml.split(' ', 1)
                        md[k] = v.strip()
                td[me.name] = md
            try:
                info = td.pop('alloc')
                labels = {
                        "jobid": job,
                        "stepid": step,
                        "user": info['user'],
                        "node": info['node'],
                }
            except KeyError:
                continue
            for tn, md in td.items():
                try:
                    mt = md.pop('time')
                except KeyError:
                    continue
                for mn, mv in md.items():
                    m = self.get_metric(mn)
                    ml = labels.copy()
                    ml['task'] = tn
                    m.add_sample(m.name, ml, mv, mt)

        return self.metrics.values()

argp = argparse.ArgumentParser(description="Prometheus exporter for slurm acct_gather_profile_exporter")
argp.add_argument('-p', '--port', metavar='NUM', type=int, default=9681, help='Listen on port NUM [9681]')
argp.add_argument('-d', '--dir', metavar='PATH', type=pathlib.Path, help='ProfileExporterDir [from scontrol show config]')
args = argp.parse_args()

jdir = args.dir
if jdir is None:
    for l in subprocess.check_output(["scontrol", "show", "config"]).split(b"\n"):
        if l.startswith(b"ProfileExporterDir"):
            n, v = l.split(b"=", 1)
            jdir = v.strip()
    if jdir is None:
        raise Exception("Could not determine ProfileExporterDir")

registry = prometheus_client.CollectorRegistry(auto_describe=True)
registry.register(Collector(jdir))

class Handler(prometheus_client.MetricsHandler.factory(registry)):
    def do_GET(self):
        if self.path == '/probe':
            self.send_response(204)
            self.end_headers()
        elif self.path == '/metrics':
            super().do_GET()
        else:
            self.send_response(404)
            self.end_headers()

HTTPServer(('', args.port), Handler).serve_forever()
