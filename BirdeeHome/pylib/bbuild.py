import sys
import os
import json
import subprocess
from queue import Queue
from threading import Thread
from threading import Lock
import locale
import traceback
import mangler 
#run %comspec% /k "D:\ProgramFiles\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" first!!

class context:
	def __init__(self):
		self.source_dirs=[]
		self.bin_search_dirs=[]
		self.root_modules=[]
		self.outpath=""
		self.link_path=[]
		self.prepared_mod=dict()
		self.link_target=None
		self.link_executable=None
		self.runtime_lib_path=""
		self.max_bin_timestamp=0
		self.num_worker_threads=1
		self.thread_worker=None
		self.link_cmd = ""

compiler_path=""
bd_home=''

LINK_EXECUTEBALE = "EXE"
LINK_SHARED = "DLL"
exe_postfix = ""
obj_postfix = '.o'
dll_symbol_list=set()
if os.name == 'nt':
	exe_postfix = ".exe"
	obj_postfix = '.obj'
	dll_postfix = '.dll'
else:
	dll_postfix = '.so'

class compile_worker:
	RUNNING=0
	DONE=1
	ABORT=2
	ABORT_DONE=3
	def __init__(self, ctx:context):
		self.q = Queue()
		def worker():
			while True:
				cu = self.q.get()
				try:
					compile_module(ctx, cu.modu,cu.source_path,cu.is_main)
					for dep_cu in cu.reverse_dependency:
						if dep_cu.source_path and dep_cu.dependency_done():
							#the module dep_cu depends on cu and it is a source code module,
							#mark the current cu as done, and check if there are other dependencies
							self.put(dep_cu)
					cu.done=True
				except Exception as e:
					#print(traceback.format_exc())
					print("\n"+str(e))
					if ctx.num_worker_threads == 1:
						print(traceback.format_exc())
					self.state=compile_worker.ABORT
				finally:
					self.q.task_done()
		for i in range(ctx.num_worker_threads):
			 t = Thread(target=worker)
			 t.daemon = True
			 t.start()
		self.joiner=Thread(target=lambda:self.q.join())
		self.state=compile_worker.RUNNING
	
	def start_joiner(self):
		self.joiner.start()

	def join(self,timeout):
		self.joiner.join(timeout)
		if self.joiner.isAlive():
			if self.state==compile_worker.RUNNING:
				return compile_worker.RUNNING
			if self.state==compile_worker.ABORT:
				return compile_worker.ABORT
			else:
				raise RuntimeError("illegal state of compile_worker")
		else:
			if self.state==compile_worker.RUNNING:
				self.state=compile_worker.DONE
			if self.state==compile_worker.ABORT:
				self.state=compile_worker.ABORT_DONE
			return self.state
	
	def put(self,cu):
		if self.state!=compile_worker.ABORT and self.state!=compile_worker.ABORT_DONE:
			self.q.put(cu)

class compile_unit:
	def __init__(self,is_main,source_path,modu):
		self.is_main=is_main
		self.source_path=source_path
		self.modu=modu
		self.reverse_dependency=set()
		self.dependency_cnt = 0
		self.lock = Lock()
		self.done = source_path is None

	def add_reverse_dependency(self,cu):
		self.reverse_dependency.add(cu)

	def get_top_level_func_name(self):
		return mangler.mangle_func(".".join(self.modu)+"_1main")

	#decrease dependency_cnt by 1, return true when this cu is ready to compile
	def dependency_done(self):
		self.lock.acquire()
		self.dependency_cnt-=1
		if self.dependency_cnt<0:
			self.lock.release()
			raise RuntimeError("Bad dependency_cnt")
		ret = self.dependency_cnt==0
		self.lock.release()
		return ret
			
	def add_dependency(self,cu):
		if cu.source_path:  #if the compile unit is a source file
			cu.add_reverse_dependency(self)
			self.dependency_cnt += 1
		#if cu is a binary module, we don't need to wait for it, so just ignore the dependency

def add_symbols(pkg, bmmdata):
		data = bmmdata
		pkgname = '.'.join(pkg) + '.'
		for var in data['Variables']:
			fstr=""
			fstr=mangler.mangle_func(pkgname+var['name'])
			dll_symbol_list.add(fstr)
		for func in data['Functions']:
			fstr=""
			if 'link_name' in func:
				continue
			else:
				fstr=mangler.mangle_func(pkgname+func['name'])
			dll_symbol_list.add(fstr)
		for clazz in data['Classes']:
			if 'funcs' in clazz:
				for funcdef in clazz['funcs']:
					if 'def' in funcdef:
						func=funcdef['def']
						fstr=mangler.mangle_func(pkgname+clazz['name']+'.'+func['name'])
						dll_symbol_list.add(fstr)

def update_max_bin_timestamp(ctx:context, fn):
	ts=os.path.getmtime(fn)
	if ts>ctx.max_bin_timestamp:
		ctx.max_bin_timestamp=ts

def get_next(idx,args):
	if idx+1>=len(args):
		raise RuntimeError("Expecting an argument after "+args[idx])
	return (idx+1,args[idx+1])
def parse_args(args)->context:
	ctx= context()
	i=1
	while i<len(args):
		if args[i]=='-i' or args[i]=='--in-source':
			i,v = get_next(i,args)
			ctx.source_dirs.append(v)
		elif args[i]=='-o' or args[i]=='--out':
			i,ctx.outpath = get_next(i,args)
		elif args[i]=='-bs' or args[i]=='--bin-search-path':
			i,v = get_next(i,args)
			ctx.bin_search_dirs.append(v)
		elif args[i]=='-le' or args[i]=='--link-executable':
			i,ctx.link_target = get_next(i,args)
			ctx.link_executable=LINK_EXECUTEBALE
		elif args[i]=='-ls' or args[i]=='--link-shared':
			i,ctx.link_target = get_next(i,args)
			ctx.link_executable=LINK_SHARED
		elif args[i]=='-lc' or args[i]=='--link-cmd':
			i,ctx.link_cmd = get_next(i,args)
		elif args[i]=='-j':
			i,v = get_next(i,args)
			ctx.num_worker_threads = int(v)
			if ctx.num_worker_threads<1:
				raise RuntimeError("Bad number of threads")
		else:
			if(args[i][0]=='-'):
				raise RuntimeError("Unknown command "+args[i])
			ctx.root_modules.append(args[i].split('.'))
		i+=1
	if len(ctx.root_modules)==0 :
		raise RuntimeError("No root modules specified")
	if len(ctx.outpath)==0 :
		ctx.outpath='.'
	ctx.bin_search_dirs.append(os.path.join(bd_home,"blib"))
	return ctx
	#if '.' not in source_dirs: source_dirs.append('.')
	#if '.' not in bin_search_dirs: bin_search_dirs.append('.')

def search_bin(ctx: context, modu):
	for path in ctx.bin_search_dirs:
		raw_path=os.path.join(path,*modu)
		p = raw_path +".bmm"
		if os.path.exists(p) and os.path.isfile(p) :
			#we have found the binary file, but if it need updating?
			src=search_src(ctx, modu)
			if src: #if it is in the source, check if source is newer than binary
				mtime_src=os.path.getmtime(src)
				mtime_bin=os.path.getmtime(p)
				if mtime_src > mtime_bin:
					return #act as if we do not find the binary file
			return raw_path
	raw_path=os.path.join(ctx.outpath,*modu)
	p = raw_path +".bmm"
	if os.path.exists(p) and os.path.isfile(p) :
		#we have found the binary file, but if it need updating?
		src=search_src(ctx,modu)
		if src: #if it is in the source, check if source is newer than binary
			mtime_src=os.path.getmtime(src)
			mtime_bin=os.path.getmtime(p)
			if mtime_src > mtime_bin:
				return #act as if we do not find the binary file
		return raw_path	

def search_src(ctx:context, modu):
	for path in ctx.source_dirs:
		raw_path=os.path.join(path,*modu)
		p = raw_path +".bdm"
		if os.path.exists(p) and os.path.isfile(p) :
			return p
		p = raw_path +".txt"
		if os.path.exists(p) and os.path.isfile(p) :
			return p

#returns if is HeaderOnly
def parse_bmm_dependency(ctx, bmmdata,self_cu):
		data=bmmdata
		dependencies=data['Imports']
		need_re_compile = False
		dep_arr = []
		for dep in dependencies:
			if dep[-1][0]==':' or dep[-1][0]=='*':  #if it is a "name import"
				dep.pop() #pop the imported name
			dep_cu = prepare_module(ctx,dep,False)
			dep_arr.append(dep_cu)
			if dep_cu.source_path: #if we need to compile a dependency, we need to re-compile self_cu
				need_re_compile = True		
		if need_re_compile:
			self_cu.source_path = "$$$???dummy"
		else:
			for dep_cu in dep_arr:
				self_cu.add_dependency(dep_cu)
		if 'HeaderOnly' in data:
			return data['HeaderOnly']
		else:
			return False

def parse_bmm_is_header_only(data):
		if 'HeaderOnly' in data:
			return data['HeaderOnly']
		else:
			return False

def create_dirs(root,modu):
	dirname=os.path.join(root,*modu[:-1])
	if not os.path.exists(dirname):
		os.makedirs(dirname)

def prepare_module(ctx:context,modu,is_main):
	tuple_modu=tuple(modu)
	if tuple_modu in ctx.prepared_mod:
		return ctx.prepared_mod[tuple_modu]
	bmm=search_bin(ctx, modu)
	cu=None
	if bmm:
		cu = compile_unit(False,None,tuple_modu)
		ctx.prepared_mod[tuple_modu] = cu
		with open(bmm+".bmm") as f:
			data = json.load(f)
		header_only = parse_bmm_dependency(ctx, data, cu)
		if not cu.source_path:
			#if we found that a dependency is never updated, we don't need to re-compile
			update_max_bin_timestamp(ctx, bmm+".bmm")
			if not header_only:#if is not header only
				ctx.link_path.append(bmm)
				add_symbols(tuple_modu, data)
			return cu
		#else re-compile the module
	#if BMM file is not found
	src=search_src(ctx, modu)
	if not src:
		raise RuntimeError("Cannot resolve module dependency: " + ".".join(modu))
	if cu: #if we have to re-compile, reuse old CU
		cu.source_path=src
		cu.is_main=is_main
	else:
		cu = compile_unit(is_main,src,tuple_modu)
	ctx.prepared_mod[tuple_modu] = cu
	update_max_bin_timestamp(ctx, src)

	cmdarr=[compiler_path,'-i',src, "--print-import"]
	cmd=" ".join(cmdarr)
	print("Running command " + cmd)
	proc = subprocess.Popen(cmdarr,stdout=subprocess.PIPE)
	dependencies_list=[]
	while True:
		line = proc.stdout.readline().decode(locale.getpreferredencoding())
		if line != '':
			dependencies_list.append(line.rstrip())
		else:
			break
	dependencies_list.pop() #delete the last element, which is the package name of the source itself
	if proc.wait()!=0:
		raise RuntimeError("Compile failed, exit code: "+ str(proc.returncode))
	proc=None #hope to release resource for the process pipe
	module_set=set()
	for depstr in dependencies_list:
		dep=depstr.split(".")
		if dep[-1][0]==':' or dep[-1][0]=='*':  #if it is a "name import"
			dep.pop() #delete the imported name
		module_set.add(tuple(dep)) #use a set to remove duplications
	for dep in module_set:
		cu.add_dependency(prepare_module(ctx, list(dep),False))
	create_dirs(ctx.outpath,modu)
	return cu


def compile_module(ctx: context, modu,src,is_main):
	outfile=os.path.join(ctx.outpath,*modu) 
	cmdarr=[compiler_path,'-i',src, "-o", outfile+ obj_postfix]
	for bpath in ctx.bin_search_dirs:
		cmdarr.append("-l")
		cmdarr.append(bpath)
	cmdarr.append("-l")
	cmdarr.append(ctx.outpath)
	#if is_main:
	#	cmdarr.append("-e")
	print("Running command " + " ".join(cmdarr))
	ret=subprocess.run(cmdarr)
	if ret.returncode!=0:
		raise RuntimeError("Compile failed")
	
	with open(outfile + '.bmm') as f:
		data = json.load(f)
	if not parse_bmm_is_header_only(data):
		ctx.link_path.append(outfile)
		add_symbols(modu, data)

def init_path():
	global bd_home,compiler_path
	bd_home=os.environ.get('BIRDEE_HOME')
	if not bd_home:
		raise RuntimeError("The environment variable BIRDEE_HOME is not set")
	compiler_path=os.path.join(bd_home,"bin","birdeec"+exe_postfix)
	if not os.path.exists(compiler_path) or not os.path.isfile(compiler_path):
		raise RuntimeError("Cannot find birdee compiler")

def link_msvc(ctx: context):
	linker_path='link.exe'
	#removed flag: /INCREMENTAL
	msvc_command='''{} /OUT:"{}" /MANIFEST /NXCOMPAT /PDB:"{}" /DYNAMICBASE {} "kernel32.lib" "user32.lib" "gdi32.lib" "winspool.lib" "comdlg32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "uuid.lib" "odbc32.lib" "odbccp32.lib" {} /DEBUG /MACHINE:X64  /SUBSYSTEM:CONSOLE /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /ManifestFile:"{}" /ERRORREPORT:PROMPT /NOLOGO /TLBID:1 '''
	ctx.runtime_lib_path = os.path.join(bd_home,"bin","BirdeeRuntime.lib")
	pdb_path= os.path.splitext(ctx.link_target)[0]+".pdb"
	obj_files='"{runtime_lib_path}"'.format(runtime_lib_path=ctx.runtime_lib_path)
	for lpath in ctx.link_path:
		lpath += obj_postfix
		obj_files += ' "{lpath}"'.format(lpath=lpath)
	cmd=msvc_command.format(linker_path,ctx.link_target,pdb_path,obj_files,ctx.link_cmd,ctx.link_target+".manifest")
	if ctx.link_executable == LINK_EXECUTEBALE:
		alias_cmd = "/alternatename:_dll_main=default_dll_main,main=" + mangler.mangle_func('.'.join(ctx.root_modules[0])) + "_0_1main"
		cmd += alias_cmd
	elif ctx.link_executable == LINK_SHARED:
		def_path = os.path.splitext(ctx.link_target)[0]+".def"
		main_func_name = mangler.mangle_func('.'.join(ctx.root_modules[0])) + "_0_1main"
		with open(def_path, 'w') as f:
			f.write("EXPORTS\n  DllMain\n")
			for itm in dll_symbol_list:
				f.write("  {}\n".format(itm))
			f.write("  {}".format(main_func_name))
		alias_cmd = "/DEF:{} /DLL /alternatename:_dll_main={}".format(def_path,main_func_name)
		cmd += alias_cmd
	print("Running command " + cmd)
	ret=subprocess.run(cmd)
	if ret.returncode!=0:
		raise RuntimeError("Compile failed")


def link_gcc(ctx:context):
	linker_path='gcc'
	cmdarr = [linker_path,'-o',ctx.link_target, "-Wl,--start-group"]
	ctx.runtime_lib_path = os.path.join(bd_home,"lib","libBirdeeRuntime.a")
	cmdarr.append(ctx.runtime_lib_path)
	for lpath in ctx.link_path:
		lpath += obj_postfix
		cmdarr.append(lpath)
	if ctx.link_executable == LINK_SHARED:
		dllmain_lib_path = os.path.join(bd_home,"lib","dllmain.cpp")
		cmdarr.append("-fPIC")
		cmdarr.append(dllmain_lib_path)
	cmdarr.append("-lgc")
	cmdarr.append("-lstdc++")
	cmdarr.append("-Wl,--end-group")
	if len(ctx.link_cmd):
		cmdarr+=ctx.link_cmd.split(" ")
	if ctx.link_executable == LINK_EXECUTEBALE:
		alias_cmd = "-Wl,--defsym,main=" + mangler.mangle_func('.'.join(ctx.root_modules[0])) + "_0_1main"
		cmdarr.append(alias_cmd)
	elif ctx.link_executable == LINK_SHARED:
		alias_cmd = "-DDLLMAIN=" + mangler.mangle_func('.'.join(ctx.root_modules[0])) + "_0_1main"
		cmdarr.append(alias_cmd)
		cmdarr.append("-shared")
	print("Running command " + ' '.join(cmdarr))
	ret=subprocess.run(cmdarr)
	if ret.returncode!=0:
		raise RuntimeError("Compile failed")


init_path()
def build(ctx: context):
	ctx.root_modules.append(['birdee'])
	file_cnt=0
	for modu in ctx.root_modules:
		is_main = file_cnt==0 and ctx.link_executable
		prepare_module(ctx,modu,is_main)
		file_cnt += 1

	ctx.thread_worker=compile_worker(ctx)
	for modu,cu in ctx.prepared_mod.items():
		if cu.source_path and cu.dependency_cnt==0: #if a module is waiting for compiling and all dependencies are resolved
			ctx.thread_worker.put(cu)
	ctx.thread_worker.start_joiner()
	while True:
		status = ctx.thread_worker.join(0.1)
		if status == compile_worker.ABORT:
			print("Aborted, waiting for tasks completion")
			while ctx.thread_worker.join(0.1)!=compile_worker.ABORT_DONE: pass
			break
		if status == compile_worker.ABORT_DONE:
			print("Aborted")
			break
		if status == compile_worker.DONE:
			break
			


	for modu,cu in ctx.prepared_mod.items():
		if not cu.done:
			raise RuntimeError("The compile unit " + ".".join(cu.modu) + " is not compiled. Dependency cnt = ", cu.dependency_cnt)

	if ctx.link_executable and ctx.link_target:
		if os.path.exists(ctx.link_target) and os.path.isfile(ctx.link_target) and  os.path.getmtime(ctx.link_target)>ctx.max_bin_timestamp:
			print("The link target is up to date")
		else:
			if os.name=='nt':
				link_msvc(ctx)
			else:
				link_gcc(ctx)


if __name__=='__main__':
	_ctx=parse_args(sys.argv)
	build(_ctx)