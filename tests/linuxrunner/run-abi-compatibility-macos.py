#!/usr/bin/env python3
"""Sequential disposable ARM64 guest ABI suite; no device/browser/GPU claims."""
import argparse,fcntl,hashlib,json,os,pathlib,shutil,subprocess
p=argparse.ArgumentParser()
for key in ['runtime','rootfs','exchange','output']:p.add_argument('--'+key,required=True)
p.add_argument('--accounts',choices=['debian','alpine'],help='Also exercise real guest account/password tools')
p.add_argument('--software-dmabuf',action='store_true',help='Enable and test experimental CPU-only buffer export in the disposable guest')
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
runtime=pathlib.Path(a.runtime).resolve();exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
report={'environment':'Mac ARM64 interpreter, not physical iPhone','runtime_sha256':hashlib.file_digest(runtime.open('rb'),'sha256').hexdigest(),'driver_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'driver_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip()),'stages':[]}
def stage(name,argv,timeout=180,env=None):
 with (out/(name+'.log')).open('w') as log:
  try:r=subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=timeout,env=env,cwd=root);code=r.returncode
  except subprocess.TimeoutExpired:code=124
 report['stages'].append({'name':name,'exit':code,'status':'PASS' if code==0 else 'FAIL'})
 (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n');print(name,code,flush=True)
 if code:raise SystemExit(code if code>0 else 1)
for name,script in [('descriptors','run-fd-integrity-macos.py'),('ancillary','run-socket-ancillary-macos.py')]:
 stage(name,['python3',str(root/'tests/linuxrunner'/script),'--runtime',str(runtime),'--rootfs',a.rootfs,'--exchange',str(exchange),'--output',str(out/name),'--host-fds'])
stage('poll-full-host',['python3',str(root/'tests/linuxrunner/run-poll-pressure-macos.py'),'--runtime',str(runtime),'--rootfs',a.rootfs,'--exchange',str(exchange),'--output',str(out/'poll-full-host')])
stage('socket-failures',['python3',str(root/'tests/linuxrunner/run-socket-failure-macos.py'),'--runtime',str(runtime),'--rootfs',a.rootfs,'--exchange',str(exchange),'--output',str(out/'socket-failures')])
stage('host-nofile',['python3',str(root/'tests/linuxrunner/run-host-nofile-macos.py'),'--runtime',str(runtime),'--rootfs',a.rootfs,'--exchange',str(exchange),'--output',str(out/'host-nofile')],timeout=600)
stage('imported-exec',['python3',str(root/'tests/linuxrunner/run-bind-exec-macos.py'),'--runtime',str(runtime),'--rootfs',a.rootfs,'--output',str(out/'imported-exec')])
with (pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 names=['exec_memory_permissions_contract','uevent_iovec_contract','uevent_netlink_contract','mkdir_existing_permissions_contract','signalfd_poll_contract','high_mmap_reuse_contract','sigpipe_contract','notsock_contract','spin_writer_contract','process_cpu_time_contract','shared_mapping_lifecycle_contract','shared_partial_page_contract','proc_maps_offsets_contract','queued_signal_contract','thread_group_exit_contract','socket_option_lifetime_contract','readlink_bounds_contract','fd_census_lifecycle_contract','poll_native_contract','udp_packet_info_contract','credentials_ipc_contract','execveat_contract','stat_empty_path_contract','dumpable_contract','proc_task_links_contract','mremap_cow','mremap_shared_memfd_contract','ofd_locks','lock_to_eof_contract','unix_halfclose_contract','unix_broker_contract','proc_fd_stat_contract','memfd_size_seals_contract','memfd_proc_reopen_contract','memfd_pressure_contract','nested_epoll_contract','mmap_offset64_contract','futex_hostfd_contract','failed_open_lifetime_contract','unix_rights_backpressure_contract','userns_contract','pidns_contract','mapping_fd_retention_contract','pty_shell_contract','termios2_contract','sync_file_range_contract','ptrace_arm64_contract','waitid_pidfd_contract','o_path_contract','fakefs_cache_coherence_contract','udp_disconnect_contract','clone_pidfd_contract','indirect_branch_contract','ptrace_parent_wait_contract','ptrace_attach_contract','precise_fault_pc_contract','opath_symlink_statx_contract','signal_return_vdso_contract','gtk_loader_ipc_contract','arena_reservation_contract']
 if a.software_dmabuf:names.insert(0,'software_dmabuf_contract')
 for name in names:shutil.copy(root/'tests/linuxrunner'/(name+'.c'),exchange)
 shutil.copytree(root/'tests/linuxrunner/elf-stack',exchange/'elf-stack',dirs_exist_ok=True)
 for src in (root/'tests/regress').glob('regress_*.c'):shutil.copy(src,exchange)
 shutil.copy(root/'tests/linuxrunner/stabilization-suite.sh',exchange)
 shutil.copy(root/'tests/linuxrunner/account_lifecycle_contract.sh',exchange)
 env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_ARM64_MEMORY_PC_FUSION=os.environ.get('LR_SUITE_MEMORY_PC_FUSION','0'))
 if a.software_dmabuf:env['ISH_SOFTWARE_DMABUF']='1'
 command='set -eu; cd /home/mobile/Downloads; sh elf-stack/run.sh; for n in '+ ' '.join(names) + '; do gcc -O2 -Wall -Wextra -pthread "$n.c" -o "$n"; "./$n"; done; for n in regress_futex_bitset regress_futex_requeue regress_futex_wait_mapping regress_futex_pi_atomic regress_futex_pi_edges regress_futex_pi_contended regress_futex_pi_stress; do gcc -O2 -pthread "$n.c" -o "$n"; "./$n"; done; gcc -O2 regress_saturating_unary.c -o saturating-unary; ./saturating-unary; gcc -O2 regress_vector_int_fixed.c -o vector-int-fixed; ./vector-int-fixed; gcc -O2 regress_mixed_saturating_accum.c -o mixed-sat; ./mixed-sat; gcc -O2 regress_saturating_shift_imm.c -o saturating-shift-imm; ./saturating-shift-imm; sh stabilization-suite.sh'
 if a.accounts:command+='; sh account_lifecycle_contract.sh'
 stage('guest-contracts',[str(runtime),'-f',a.rootfs,'/bin/sh','-ec',command],timeout=420,env=env)
if a.accounts:stage('account-tools',['python3',str(root/'tests/linuxrunner/run-account-tools-macos.py'),'--runtime',str(runtime),'--rootfs',a.rootfs,'--distribution',a.accounts,'--output',str(out/'account-tools')],timeout=240)
print('ABI_COMPATIBILITY_SUITE_PASS; browser, GUI and GPU tests are separate')
