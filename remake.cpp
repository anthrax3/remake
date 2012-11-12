/**
@mainpage Remake, a build system that bridges the gap between make and redo.

As with <b>make</b>, <b>remake</b> uses a centralized rule file, which is
named <b>Remakefile</b>. It contains rules with a <em>make</em>-like
syntax:

@verbatim
target1 target2 ... : dependency1 dependency2 ...
	shell script
	that builds
	the targets
@endverbatim

A target is known to be up-to-date if all its dependencies are. If it
has no known dependencies yet the file already exits, it is assumed to
be up-to-date. Obsolete targets are rebuilt thanks to the shell script
provided by the rule.

As with <b>redo</b>, <b>remake</b> supports dynamic dependencies in
addition to these static dependencies. Whenever a script executes
<tt>remake dependency4 dependency5 ...</tt>, these dependencies are
rebuilt if they are obsolete. (So <b>remake</b> acts like
<b>redo-ifchange</b>.) Moreover, these dependencies are stored in file
<b>.remake</b> so that they are remembered in subsequent runs. Note that
dynamic dependencies from previous runs are only used to decide whether a
target is obsolete; they are not automatically rebuilt when they are
obsolete yet a target depends on them. They will only be rebuilt once the
dynamic call to <b>remake</b> is executed.

In other words, the following two rules have almost the same behavior.

@verbatim
target1 target2 ... : dependency1 dependency2 ...
	shell script

target1 target2 ... :
	remake dependency1 dependency2 ...
	shell script
@endverbatim

(There is a difference if the targets already exist, have never been
built before, and the dependencies are either younger or obsolete, since
the targets will not be rebuilt in the second case.)

The above usage of dynamic dependencies is hardly useful. Their strength
lies in the fact that they can be computed on the fly:

@verbatim
%.o : %.c
	gcc -MMD -MF $1.d -o $1 -c ${1%.o}.c
	read DEPS < $1.d
	remake ${DEPS#*:}
	rm $1.d

%.cmo : %.ml
	remake $(ocamldep ${1%.cmo}.ml | sed -n -e "\\,^.*: *\$, b; \\,$1:, { b feed2; :feed1 N; :feed2 s/[\\]\$//; t feed1; s/.*://; s/[ \\t\\r\\n]*\\([ \\t\\r\\n]\\+\\)/\\1\n/g; s/\\n\$//; p; q}")
	ocamlc -c ${1%.cmo}.ml
@endverbatim

Note that the first rule fails if any of the header files included by
a C source file has to be automatically generated. In that case, one
should perform a first call to <b>remake</b> them before calling the
compiler. (Dependencies from several calls to <b>remake</b> are
cumulative, so they will all be remembered the next time.)

Options:
- <tt>-j[N]</tt>, <tt>--jobs=[N]</tt>: Allow N jobs at once; infinite jobs
  with no argument.

Other differences with <b>make</b>:

- For rules with multiple targets, the shell script is executed only once
  and is assumed to build all the targets. There is no need for
  convoluted rules that are robust enough for parallel builds.
- As with <b>redo</b>, only one shell is run when executing a script,
  rather than one per script line. Note that the shells are run with
  option <tt>-e</tt>, thus causing them to exit as soon as an error is
  encountered.
- The dependencies of generic rules (known as implicit rules in make lingo)
  are not used to decide between several of them. <b>remake</b> does not
  select one for which it could satisfy the dependencies.
- <b>remake</b> has almost no features: no variables, no predefined
  functions, etc.

Other differences with <b>redo</b>:

- As with <b>make</b>, it is possible to write the following kind of rules
  in <b>remake</b>.
@verbatim
Remakefile: Remakefile.in ./config.status
	./config.status Remakefile
@endverbatim
- <b>remake</b> has almost no features: no checksum-based dependencies, no
  compatibility with token servers, etc.

Other differences with <b>make</b> and <b>redo</b>:

- When executing shell scripts, positional variables <tt>$1</tt>,
  <tt>$2</tt>, etc, point to the target names of the rule obtained after
  substituting <tt>%</tt>. No other variables are defined.

Limitations:

- When the user or a script calls <b>remake</b>, the current working
  directory should be the one containing <b>Remakefile</b> (and thus
  <b>.remake</b> too). This is unavoidable for user calls, but could be
  improved for recursive calls.
- Target names are not yet normalized, so <tt>f</tt> and <tt>d/../f</tt>
  are two different targets.

@see http://cr.yp.to/redo.html for the philosophy of <b>redo</b> and
https://github.com/apenwarr/redo for an implementation and some comprehensive documentation.

@author Guillaume Melquiond
@version 0.1
@date 2012
@copyright
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
\n
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

typedef std::list<std::string> string_list;

typedef std::set<std::string> string_set;

typedef std::map<std::string, string_set> dependency_map;

/**
 * Build status of a target.
 */
enum status_e
{
	Uptodate, ///< Target is up-to-date.
	Todo,     ///< Target is missing or obsolete.
	Running,  ///< Target is being rebuilt.
	Remade,   ///< Target was successfully rebuilt.
	Failed    ///< Build failed for target.
};

/**
 * Build status of a target and last-modified date for up-to-date targets.
 */
struct status_t
{
	status_e status; ///< Actual status.
	time_t last;     ///< Last-modified date.
};

typedef std::map<std::string, status_t> status_map;

/**
 * A rule loaded from Remakefile.
 */
struct rule_t
{
	bool generic;        ///< Whether the filenames contain placeholders.
	string_list targets; ///< Files produced by this rule.
	string_list deps;    ///< Files used for an implicit call to remake at the start of the script.
	std::string script;  ///< Shell script for building the targets.
	rule_t(): generic(false) {}
};

typedef std::list<rule_t> rule_list;

typedef std::map<int, string_list> job_targets_map;

typedef std::map<pid_t, int> pid_job_map;

/**
 * Client waiting for a request complete.
 *
 * There are two kinds of clients:
 * - real clients, which are instances of remake created by built scripts,
 * - pseudo clients, which are created by the server to build specific targets.
 *
 * Among pseudo clients, there are two categories:
 * - original clients, which are created for the targets passed on the
 *   command line by the user or for the initial regeneration of the rule file,
 * - dependency clients, which are created to handle rules that have
 *   explicit dependencies and thus to emulate a call to remake.
 */
struct client_t
{
	int fd;              ///< File descriptor used to reply to the client (negative for pseudo clients).
	int job_id;          ///< Job for which the built script called remake and spawned the client (negative for original clients).
	string_list pending; ///< Targets not yet started.
	string_set running;  ///< Targets being built.
	rule_t *delayed;     ///< Rule that implicitly created a dependency client, and which script has to be started on request completion.
	client_t(): fd(-1), job_id(-1), delayed(NULL) {}
};

typedef std::list<client_t> client_list;

/**
 * Map from targets to their known dependencies.
 */
static dependency_map deps;

/**
 * Map from targets to their build status.
 */
static status_map status;

/**
 * Set of loaded rules.
 */
static rule_list rules;

/**
 * Map from jobs to targets being built.
 */
static job_targets_map job_targets;

/**
 * Map from jobs to shell pids.
 */
static pid_job_map job_pids;

/**
 * List of clients waiting for a request to complete.
 * New clients are put to front, so that the build process is depth-first.
 */
static client_list clients;

/**
 * Maximum number of parallel jobs (non-positive if unbounded).
 * Can be modified by the -j option.
 */
static int max_active_jobs = 1;

/**
 * Number of jobs currently running:
 * - it increases when a process is created in #run_script,
 * - it decreases when a completion message is received in #finalize_job.
 *
 * @note There might be some jobs running while #clients is empty.
 *       Indeed, if a client requested two targets to be rebuilt, if they
 *       are running concurrently, if one of them fails, the client will
 *       get a failure notice and might terminate before the other target
 *       finishes.
 */
static int running_jobs = 0;

/**
 * Number of jobs currently waiting for a build request to finish:
 * - it increases when a build request is received in #accept_client
 *   (since the client is presumably waiting for the reply),
 * - it decreases when a reply is sent in #complete_request.
 */
static int waiting_jobs = 0;

/**
 * Global counter used to produce increasing job numbers.
 * @see job_targets
 */
static int job_counter = 0;

/**
 * Socket on which the server listens for client request.
 */
static int socket_fd;

/**
 * Whether the request of an original client failed.
 */
static bool build_failure;

/**
 * Name of the server socket in the file system.
 */
static char *socket_name;

static volatile sig_atomic_t got_SIGCHLD = 0;

struct log
{
	bool active, open;
	int depth;
	log(): active(false), open(false), depth(0)
	{
	}
	std::ostream &operator()()
	{
		if (open) std::cerr << std::endl;
		assert(depth >= 0);
		std::cerr << std::string(depth * 2, ' ');
		open = false;
		return std::cerr;
	}
	std::ostream &operator()(bool o)
	{
		if (o && open) std::cerr << std::endl;
		if (!o) --depth;
		assert(depth >= 0);
		if (o || !open) std::cerr << std::string(depth * 2, ' ');
		if (o) ++depth;
		open = o;
		return std::cerr;
	}
};

log debug;

struct log_auto_close
{
	bool still_open;
	log_auto_close(): still_open(true)
	{
	}
	~log_auto_close()
	{
		if (debug.active && still_open) debug(false) << "done\n";
	}
};

#define DEBUG if (debug.active) debug()
#define DEBUG_open log_auto_close auto_close; if (debug.active) debug(true)
#define DEBUG_close if ((auto_close.still_open = false), debug.active) debug(false)

/**
 * Return the original string if it does not contain any special characters,
 * a quoted and escaped string otherwise.
 */
static std::string escape_string(std::string const &s)
{
	size_t len = s.length(), nb = len;
	for (size_t i = 0; i < len; ++i)
	{
		if (strchr("\" \\$!", s[i])) ++nb;
	}
	if (nb == len) return s;
	std::string t(nb + 2, '\\');
	t[0] = '"';
	for (size_t i = 0, j = 1; i < len; ++i, ++j)
	{
		if (strchr("\" \\$!", s[i])) ++j;
		t[j] = s[i];
	}
	t[nb + 1] = '"';
	return t;
}

/**
 * Skip spaces.
 */
static void skip_spaces(std::istream &in)
{
	char c;
	while ((c = in.get()) == ' ') {}
	if (in.good()) in.putback(c);
}

/**
 * Skip end of line.
 */
static void skip_eol(std::istream &in)
{
	char c;
	while (strchr("\r\n", (c = in.get()))) {}
	if (in.good()) in.putback(c);
}

/**
 * Read a (possibly quoted) word.
 */
static std::string read_word(std::istream &in)
{
	int c = in.get();
	std::string res;
	if (!in.good()) return res;
	if (strchr(" \t\r\n:", c))
	{
		in.putback(c);
		return res;
	}
	bool quoted = c == '"';
	if (!quoted) res += c;
	while (true)
	{
		c = in.get();
		if (!in.good()) return res;
		if (quoted)
		{
			if (c == '\\')
				res += in.get();
			else if (c == '"')
				return res;
			else
				res += c;
		}
		else
		{
			if (strchr(" \t\r\n:", c))
			{
				in.putback(c);
				return res;
			}
			res += c;
		}
	}
}

/**
 * Load known dependencies from file <tt>.remake</tt>.
 */
static void load_dependencies()
{
	DEBUG_open << "Loading database... ";
	std::ifstream in(".remake");
	if (!in.good())
	{
		DEBUG_close << "not found\n";
		return;
	}
	while (!in.eof())
	{
		std::string target = read_word(in);
		if (target.empty()) return;
		DEBUG << "reading dependencies of target " << target << std::endl;
		if (in.get() != ':')
		{
			std::cerr << "Failed to load database" << std::endl;
			exit(1);
		}
		std::string dep;
		skip_spaces(in);
		while (!(dep = read_word(in)).empty())
		{
			DEBUG << "adding " << dep << " as dependency\n";
			deps[target].insert(dep);
			skip_spaces(in);
		}
		skip_eol(in);
	}
}

/**
 * Save all the dependencies in file <tt>.remake</tt>.
 */
static void save_dependencies()
{
	DEBUG_open << "Saving database... ";
	std::ofstream db(".remake");
	for (dependency_map::const_iterator i = deps.begin(),
	     i_end = deps.end(); i != i_end; ++i)
	{
		if (i->second.empty()) continue;
		db << escape_string(i->first) << ": ";
		for (string_set::const_iterator j = i->second.begin(),
		     j_end = i->second.end(); j != j_end; ++j)
		{
			db << escape_string(*j) << ' ';
		}
		db << std::endl;
	}
}

/**
 * Internal state of the #load_rules parser.
 */
enum load_state_e { Bof, Tgt, Dep, Script };

/**
 * Load rules.
 * If some rules have dependencies and non-generic targets, add these
 * dependencies to the targets.
 */
static void load_rules()
{
	DEBUG_open << "Loading rules... ";
	int line = 1;
	if (false)
	{
		error:
		std::cerr << "Failed to load rules: syntax error at line" << line << std::endl;
		exit(1);
	}
	std::ifstream in("Remakefile");
	if (!in.good())
	{
		std::cerr << "Failed to load rules: no Remakefile found" << std::endl;
		exit(1);
	}
	rule_t current;
	std::ostringstream buf;
	load_state_e state = Bof;
	while (!in.eof())
	{
		int c = in.get();
		if (in.eof()) break;
		if (state == Script && c == '\t')
		{
			in.get(*buf.rdbuf());
		}
		else if (state == Script && (c == '\r' || c == '\n'))
		{
			buf << (char)c;
			if (c == '\n') ++line;
		}
		else if (state == Dep && c == '\n')
		{
			++line;
			state = Script;
		}
		else if (state == Tgt && c == ':')
		{
			state = Dep;
			skip_spaces(in);
		}
		else
		{
			if (state == Script)
			{
				DEBUG << "adding rule for target " << current.targets.front() << std::endl;
				current.script = buf.str();
				rules.push_back(current);
				buf.str(std::string());
				current = rule_t();
			}
			in.putback(c);
			std::string file = read_word(in);
			skip_spaces(in);
			if (file.empty()) goto error;
			if (file.find('%') != std::string::npos)
			{
				if ((state == Tgt || state == Dep) && !current.generic)
					goto error;
				current.generic = true;
			}
			else if (state == Tgt && current.generic) goto error;
			if (state != Dep)
			{
				current.targets.push_back(file);
				state = Tgt;
				continue;
			}
			current.deps.push_back(file);
			if (current.generic) continue;
			for (string_list::const_iterator i = current.targets.begin(),
			     i_end = current.targets.end(); i != i_end; ++i)
			{
				deps[*i].insert(file);
			}
		}
	}
	if (state != Bof)
	{
		DEBUG << "adding rule for target " << current.targets.front() << std::endl;
		current.script = buf.str();
		rules.push_back(current);
	}
}

/**
 * Substitute a pattern into a list of strings.
 */
static void substitute_pattern(std::string const &pat, string_list const &src, string_list &dst)
{
	for (string_list::const_iterator i = src.begin(),
	     i_end = src.end(); i != i_end; ++i)
	{
		size_t pos = i->find('%');
		if (pos == std::string::npos)dst.push_back(*i);
		else dst.push_back(i->substr(0, pos) + pat + i->substr(pos + 1));
	}
}

/**
 * Find a rule matching @a target:
 * - non-generic rules have priority,
 * - among generic rules, the one leading to shorter matches have priority,
 * - among several rules, the earliest one has priority.
 */
static rule_t find_rule(std::string const &target)
{
	size_t plen = 10000, tlen = target.length();
	rule_t rule;
	for (rule_list::const_iterator i = rules.begin(),
	     i_end = rules.end(); i != i_end; ++i)
	{
		for (string_list::const_iterator j = i->targets.begin(),
		     j_end = i->targets.end(); j != j_end; ++j)
		{
			if (!i->generic)
			{
				if (*j == target) return *i;
				else continue;
			}
			size_t len = j->length();
			if (tlen < len) continue;
			if (plen <= tlen - (len - 1)) continue;
			size_t pos = j->find('%');
			if (pos == std::string::npos) continue;
			size_t len2 = len - (pos + 1);
			if (j->compare(0, pos, target, 0, pos) ||
			    j->compare(pos + 1, len2, target, tlen - len2, len2))
				continue;
			plen = tlen - (len - 1);
			std::string pat = target.substr(pos, plen);
			rule = rule_t();
			rule.script = i->script;
			substitute_pattern(pat, i->targets, rule.targets);
			substitute_pattern(pat, i->deps, rule.deps);
			break;
		}
	}
	return rule;
}

/**
 * Compute and memoize the status of @a target:
 * - if the file does not exist, the target is obsolete,
 * - if any dependency is obsolete or younger than the file, it is obsolete,
 * - otherwise it is up-to-date.
 */
static status_t const &get_status(std::string const &target)
{
	std::pair<status_map::iterator,bool> i =
		status.insert(std::make_pair(target, status_t()));
	if (!i.second) return i.first->second;
	DEBUG_open << "Checking status of " << target << "... ";
	struct stat s;
	status_t &ts = i.first->second;
	if (stat(target.c_str(), &s) != 0)
	{
		obsolete:
		ts.status = Todo;
		DEBUG_close << "obsolete\n";
		return ts;
	}
	string_set const &dep = deps[target];
	for (string_set::const_iterator k = dep.begin(),
	     k_end = dep.end(); k != k_end; ++k)
	{
		status_t const &ts = get_status(*k);
		if (ts.status != Uptodate || ts.last > s.st_mtime)
			goto obsolete;
	}
	ts.status = Uptodate;
	ts.last = s.st_mtime;
	DEBUG_close << "up-to-date\n";
	return ts;
}

/**
 * Handle job completion.
 */
static void complete_job(int job_id, bool success)
{
	DEBUG_open << "Completing job " << job_id << "... ";
	job_targets_map::iterator i = job_targets.find(job_id);
	assert(i != job_targets.end());
	string_list const &targets = i->second;
	if (success)
	{
		for (string_list::const_iterator j = targets.begin(),
		     j_end = targets.end(); j != j_end; ++j)
		{
			status[*j].status = Remade;
		}
	}
	else
	{
		DEBUG_close << "failed\n";
		std::cerr << "Failed to build";
		for (string_list::const_iterator j = targets.begin(),
		     j_end = targets.end(); j != j_end; ++j)
		{
			status[*j].status = Failed;
			std::cerr << ' ' << *j;
			remove(j->c_str());
		}
		std::cerr << std::endl;
	}
	job_targets.erase(i);
}

static void child_sig_handler(int sig)
{
	got_SIGCHLD = 1;
}

/**
 * Execute the script from @a rule.
 */
static void run_script(int job_id, rule_t const &rule)
{
	DEBUG_open << "Starting script for job " << job_id << "... ";
	if (pid_t pid = fork())
	{
		if (pid == -1)
		{
			DEBUG_close << "failed\n";
			complete_job(job_id, false);
			return;
		}
		++running_jobs;
		job_pids[pid] = job_id;
		return;
	}
	std::ostringstream buf;
	buf << job_id;
	if (setenv("REMAKE_JOB_ID", buf.str().c_str(), 1))
		_exit(1);
	char const **argv = new char const *[6 + rule.targets.size()];
	argv[0] = "sh";
	argv[1] = "-e";
	argv[2] = "-c";
	argv[3] = rule.script.c_str();
	argv[4] = "remake-shell";
	int num = 5;
	for (string_list::const_iterator i = rule.targets.begin(),
	     i_end = rule.targets.end(); i != i_end; ++i, ++num)
	{
		argv[num] = i->c_str();
	}
	argv[num] = NULL;
	execv("/bin/sh", (char **)argv);
	_exit(1);
}

/**
 * Create a job for @a target according to the loaded rules.
 * Mark all the targets from the rule as running and reset their dependencies.
 * If the rule has dependencies, create a new client to build them just
 * before @a current, and change @a current so that it points to it.
 */
static bool start(std::string const &target, client_list::iterator &current)
{
	DEBUG_open << "Starting job " << job_counter << " for " << target << "... ";
	rule_t rule = find_rule(target);
	if (rule.targets.empty())
	{
		status[target].status = Failed;
		DEBUG_close << "failed\n";
		std::cerr << "No rule for building " << target << std::endl;
		return false;
	}
	for (string_list::const_iterator i = rule.targets.begin(),
	     i_end = rule.targets.end(); i != i_end; ++i)
	{
		status[*i].status = Running;
		string_set &dep = deps[*i];
		dep.clear();
		dep.insert(rule.deps.begin(), rule.deps.end());
	}
	if (!rule.deps.empty())
	{
		current = clients.insert(current, client_t());
		current->job_id = job_counter;
		std::swap(current->pending, rule.deps);
		current->delayed = new rule_t(rule);
	}
	else run_script(job_counter, rule);
	job_targets[job_counter] = rule.targets;
	++job_counter;
	return true;
}

/**
 * Send a reply to a client then remove it.
 * If the client was a dependency client, start the actual script.
 */
static void complete_request(client_t &client, bool success)
{
	DEBUG_open << "Completing request from client of job " << client.job_id << "... ";
	if (client.delayed)
	{
		assert(client.fd < 0);
		if (success) run_script(client.job_id, *client.delayed);
		else complete_job(client.job_id, false);
		delete client.delayed;
	}
	else if (client.fd >= 0)
	{
		char res = success ? 1 : 0;
		send(client.fd, &res, 1, 0);
		close(client.fd);
		--waiting_jobs;
	}

	if (client.job_id < 0 && !success) build_failure = true;
}

/**
 * Return whether there are slots for starting new jobs.
 */
static bool has_free_slots()
{
	if (max_active_jobs <= 0) return true;
	return running_jobs - waiting_jobs < max_active_jobs;
}

/**
 * Update clients as long as there are free slots:
 * - check for running targets that have finished,
 * - start as many pending targets as allowed,
 * - complete the request if there are neither running nor pending targets
 *   left or if any of them failed.
 */
static void update_clients()
{
	DEBUG_open << "Updating clients... ";
	for (client_list::iterator i = clients.begin(), i_next = i,
	     i_end = clients.end(); i != i_end && has_free_slots(); i = i_next)
	{
		++i_next;
		DEBUG_open << "Handling client from job " << i->job_id << "... ";
		if (false)
		{
			failed:
			complete_request(*i, false);
			clients.erase(i);
			DEBUG_close << "failed\n";
			continue;
		}

		// Remove running targets that have finished.
		for (string_set::iterator j = i->running.begin(), j_next = j,
		     j_end = i->running.end(); j != j_end; j = j_next)
		{
			++j_next;
			status_map::const_iterator k = status.find(*j);
			assert(k != status.end());
			switch (k->second.status)
			{
			case Uptodate:
			case Remade:
				i->running.erase(j);
			case Running:
				break;
			case Todo:
				assert(false);
			case Failed:
				goto failed;
			}
		}

		// Start pending targets.
		while (!i->pending.empty())
		{
			std::string target = i->pending.front();
			i->pending.pop_front();
			switch (get_status(target).status)
			{
			case Failed:
				goto failed;
			case Running:
				i->running.insert(target);
				break;
			case Uptodate:
			case Remade:
				break;
			case Todo:
				client_list::iterator j = i;
				if (!start(target, i)) goto failed;
				j->running.insert(target);
				if (!has_free_slots()) return;
				// Job start might insert a dependency client.
				i_next = i;
				++i_next;
				break;
			}
		}

		// Try to complete request.
		// (This might start a new job if it was a dependency client.)
		if (i->running.empty())
		{
			complete_request(*i, true);
			clients.erase(i);
			DEBUG_close << "finished\n";
		}
	}
}

/**
 * Create a named unix socket that listens for build requests. Also set
 * the REMAKE_SOCKET environment variable that will be inherited by all
 * the job scripts.
 */
static void create_server()
{
	if (false)
	{
		error:
		perror("Failed to create server");
		error2:
		exit(1);
	}
	DEBUG_open << "Creating server... ";

	// Set a handler for SIGCHLD then block the signal (unblocked during select).
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) == -1) goto error;
	struct sigaction sa;
	sa.sa_flags = 0;
	sa.sa_handler = &child_sig_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) == -1) goto error;

	// Prepare a named unix socket in temporary directory.
	socket_name = tempnam(NULL, "rmk-");
	if (!socket_name) goto error2;
	struct sockaddr_un socket_addr;
	size_t len = strlen(socket_name);
	if (len >= sizeof(socket_addr.sun_path) - 1) goto error2;
	socket_addr.sun_family = AF_UNIX;
	strcpy(socket_addr.sun_path, socket_name);
	len += sizeof(socket_addr.sun_family);
	if (setenv("REMAKE_SOCKET", socket_name, 1)) goto error;

	// Create and listen to the socket.
	socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (socket_fd < 0) goto error;
	if (bind(socket_fd, (struct sockaddr *)&socket_addr, len))
		goto error;
	if (listen(socket_fd, 1000)) goto error;
}

/**
 * Accept a connection from a client, get the job it spawned from,
 * get the targets, and mark them as dependencies of the job targets.
 */
void accept_client()
{
	DEBUG_open << "Handling client request... ";

	// Accept connection.
	struct sockaddr_un remote;
	socklen_t len = sizeof(remote);
	int fd = accept4(socket_fd, (struct sockaddr *)&remote, &len, SOCK_CLOEXEC);
	if (fd < 0) return;

	clients.push_front(client_t());
	client_list::iterator proc = clients.begin();

	if (false)
	{
		error:
		DEBUG_close << "failed\n";
		std::cerr << "Received an ill-formed client message" << std::endl;
		close(fd);
		clients.erase(proc);
		return;
	}

	// Receive job that spawned the client.
	int job_id;
	if (recv(fd, &job_id, sizeof(job_id), 0) != sizeof(job_id))
		goto error;
	proc->job_id = job_id;
	proc->fd = fd;
	job_targets_map::const_iterator i = job_targets.find(job_id);
	if (i == job_targets.end()) goto error;
	DEBUG << "receiving request from job " << job_id << std::endl;

	// Receive targets the client wants to build.
	std::ostringstream tbuf;
	while (true)
	{
		char buf[1024];
		ssize_t len = recv(fd, &buf, 1024, 0);
		if (len <= 0) goto error;
		tbuf.write(buf, len);
		if (buf[len - 1] == 0)
		{
			std::string const &targets = tbuf.str();
			size_t len = targets.length();
			if (len >= 2 && targets[len - 2] == 0) break;
		}
	}

	// Parse the targets and mark them as dependencies from the job targets.
	std::string const &targets = tbuf.str();
	char const *p = targets.c_str(), *p_end = p + targets.length() + 1;
	while (p != p_end)
	{
		size_t len = strlen(p);
		if (len == 0)
		{
			++waiting_jobs;
			return;
		}
		std::string target(p, p + len);
		DEBUG << "adding dependency " << target << " to job\n";
		proc->pending.push_back(target);
		string_list const &l = job_targets[job_id];
		for (string_list::const_iterator i = l.begin(),
		     i_end = l.end(); i != i_end; ++i)
		{
			deps[*i].insert(target);
		}
		p += len + 1;
	}
	goto error;
}

/**
 * Loop until all the jobs have finished.
 */
void server_loop()
{
	while (true)
	{
		update_clients();
		if (running_jobs == 0)
		{
			assert(clients.empty());
			break;
		}
		DEBUG_open << "Handling events... ";
		sigset_t emptymask;
		sigemptyset(&emptymask);
		fd_set fdset;
		FD_ZERO(&fdset);
		FD_SET(socket_fd, &fdset);
		int ret = pselect(socket_fd + 1, &fdset, NULL, NULL, NULL, &emptymask);
		if (ret > 0 /* && FD_ISSET(socket_fd, &fdset)*/) accept_client();
		if (!got_SIGCHLD) continue;
		got_SIGCHLD = 0;
		pid_t pid;
		int status;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		{
			bool res = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			pid_job_map::iterator i = job_pids.find(pid);
			assert(i != job_pids.end());
			int job_id = i->second;
			job_pids.erase(i);
			--running_jobs;
			complete_job(job_id, res);
		}
	}
}

/**
 * Load dependencies and rules, listen to client requests, and loop until
 * all the requests have completed.
 * If Remakefile is obsolete, perform a first run with it only, then reload
 * the rules, and perform a second with the original clients.
 */
void server_mode(string_list const &targets)
{
	load_dependencies();
	load_rules();
	create_server();
	if (get_status("Remakefile").status == Todo)
	{
		clients.push_back(client_t());
		clients.back().pending.push_back("Remakefile");
		server_loop();
		if (build_failure) goto early_exit;
		rules.clear();
		load_rules();
	}
	clients.push_back(client_t());
	clients.back().pending = targets;
	server_loop();
	early_exit:
	close(socket_fd);
	remove(socket_name);
	save_dependencies();
	exit(build_failure ? 1 : 0);
}

/**
 * Connect to the server @a socket_name, send a build request for @a targets,
 * and exit with the status returned by the server.
 */
void client_mode(char *socket_name, string_list const &targets)
{
	if (false)
	{
		error:
		perror("Failed to send targets to server");
		exit(1);
	}
	if (targets.empty()) exit(0);
	DEBUG_open << "Connecting to server... ";

	// Connect to server.
	struct sockaddr_un socket_addr;
	size_t len = strlen(socket_name);
	if (len >= sizeof(socket_addr.sun_path) - 1) exit(1);
	socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_fd < 0) goto error;
	socket_addr.sun_family = AF_UNIX;
	strcpy(socket_addr.sun_path, socket_name);
	if (connect(socket_fd, (struct sockaddr *)&socket_addr, sizeof(socket_addr.sun_family) + len))
		goto error;

	// Send current job id.
	char *id = getenv("REMAKE_JOB_ID");
	int job_id = id ? atoi(id) : -1;
	if (send(socket_fd, &job_id, sizeof(job_id), MSG_NOSIGNAL) != sizeof(job_id))
		goto error;

	// Send tagets.
	for (string_list::const_iterator i = targets.begin(),
	     i_end = targets.end(); i != i_end; ++i)
	{
		DEBUG_open << "Sending " << *i << "... ";
		ssize_t len = i->length() + 1;
		if (send(socket_fd, i->c_str(), len, MSG_NOSIGNAL) != len)
			goto error;
	}

	// Send terminating nul and wait for reply.
	char result = 0;
	if (send(socket_fd, &result, 1, MSG_NOSIGNAL) != 1) goto error;
	if (recv(socket_fd, &result, 1, 0) != 1) exit(1);
	exit(result ? 0 : 1);
}

/**
 * Display usage and exit with @a exit_status.
 */
void usage(int exit_status)
{
	std::cerr << "Usage: remake [options] [target] ...\n"
		"Options\n"
		"  -d                 Print lots of debugging information.\n"
		"  -h, --help         Print this message and exit.\n"
		"  -j[N], --jobs=[N]  Allow N jobs at once; infinite jobs with no arg.\n";
	exit(exit_status);
}

/**
 * This program behaves in two different ways.
 *
 * - If the environment contains the REMAKE_SOCKET variable, the client
 *   connects to this socket and sends to the server its build targets.
 *   It exits once it receives the server reply.
 *
 * - Otherwise, it creates a server that waits for build requests. It
 *   also creates a pseudo-client that requests the targets passed on the
 *   command line.
 */
int main(int argc, char *argv[])
{
	string_list targets;

	// Parse command-line arguments.
	for (int i = 1; i < argc; ++i)
	{
		std::string arg = argv[i];
		if (arg.empty()) usage(1);
		if (arg == "-h" || arg == "--help") usage(0);
		if (arg == "-d")
			debug.active = true;
		else if (arg.compare(0, 2, "-j") == 0)
			max_active_jobs = atoi(arg.c_str() + 2);
		else if (arg.compare(0, 7, "--jobs=") == 0)
			max_active_jobs = atoi(arg.c_str() + 7);
		else
		{
			if (arg[0] == '-') usage(1);
			targets.push_back(arg);
			DEBUG << "New target: " << arg << '\n';
		}
	}

	// Run as client if REMAKE_SOCKET is present in the environment.
	if (char *sn = getenv("REMAKE_SOCKET")) client_mode(sn, targets);

	// Otherwise run as server.
	server_mode(targets);
}
