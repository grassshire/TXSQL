/** Copyright (c) 2025, Chengdu Haiguang IC Design Co., Ltd.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef NUMA_AFFINITY
#define NUMA_AFFINITY

#include "my_inttypes.h"
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <vector>
#include <sched.h>

#define NUMA_AFFINITY_MAX_NODE_NUM 64
#define NUMA_AFFINITY_BIG_BUF_SIZE 4096
#define NUMA_AFFINITY_CONVERT_DIGITS_TO_NUM(p, n) \
  do { \
    n = *p++ - '0'; \
    while (isdigit(*p)) { \
      n *= 10; \
      n += (*p++ - '0'); \
    } \
  } while(0)

class mysql_server_numa_infos;
extern mysql_server_numa_infos m_server_numa_infos;

typedef struct {
  /* Cpu list on current node. */
  cpu_set_t cpu_set;

  /* Distances to different nodes from current node. */
  std::vector<int> distance;
} node_data_t;

typedef struct {
  /* Cpu list on current node. */
  cpu_set_t cpu_set;

  /* How many cpu resource allocated on current node for mysql server process. */
  int cpu_num;
} process_node_data_t;

/* Manage system numa node information and numa node available for mysql main process.
   At the same time, provide interfaces for binding/unbinding thread and updating node resource. */
class mysql_server_numa_infos {
public:
  mysql_server_numa_infos() {}
  ~mysql_server_numa_infos() {}

  void init(void) {
    if (get_nodes_info()) {
      return;
    }

    if (get_process_sched_info()) {
      return;
    }

    m_inited = true;
  }

  /**
    OR operation of two cpu_set_t objects.

    @param  dest  The destination object.
    @param  src1  The first operating object.
    @param  src2  The second operating object.
  */
  void cpu_set_or(cpu_set_t *dest, cpu_set_t *src1,  cpu_set_t *src2);

  /**
    Find nodeX in the spefific directory.

    @param  dptr  Pointer to structure describing a directory path.
    @return 1 if it's a directory name starting with 'node'.
            0 otherwise.
  */
  static int node_and_digits(const struct dirent *dptr);

  /**
    Do OR operation of two cpu_set_t objects.

    @param  list    List to add ids to.
    @param  num_str String describing a node index.
  */
  int add_ids_to_list_from_str(cpu_set_t *list, uchar *ids_str);

  /**
    Fetch node information from system filesystem, including the numa node number and cpu list per node.
    @return 0 if getting system numa node information successfully.
            1 otherwise.
  */
  int get_nodes_info(void);

  /**
    Fetch available numa node for the main process, including available numa node number and available cpu list per node.
    @return 0 if get numa node information for main process successfully.
            1 otherwise.
  */
  int get_process_sched_info(void);

  /**
    Update available cpu list according to the configured node number.
    @param  node_num        The number of nodes to configure or re-configure.
    @param  cpuset_affinity Storing cpu list assigned according to node_num.
  */
  void update_affinity_cpuset(int node_num, cpu_set_t *cpuset_affinity);

  /**
    Set numa affinity for the caller.
    @param  cpuset_affinity Cpu list binding the caller to. 
  */
  void set_numa_affinity(cpu_set_t *cpuset_affinity);

  /**
    Unset numa affinity of the caller. In fact, this function binds the caller to cpu list the main process binding in.
  */
  void unset_numa_affinity(void);

  bool is_inited(void) { return m_inited; }

private:
  /* The number of nodes in the machine. */
  int m_nodes_num = 0;
  node_data_t m_nodes_data[NUMA_AFFINITY_MAX_NODE_NUM];

  /* CPU list that is allocated for mysql main process. */
  cpu_set_t m_process_cpuset;
  int m_process_nodes_num = 0;

  process_node_data_t m_process_nodes_data[NUMA_AFFINITY_MAX_NODE_NUM];

  /* The node index on which node mysql main process has the most allocated cpu resource.
     If at least two nodes have the same resource, here is the smallest index of them.
  */
  int m_process_rich_node_idx;

  bool m_inited = false;
};

class numa_affinity {
public:
  numa_affinity() {
    m_numa_infos = (mysql_server_numa_infos *)(&m_server_numa_infos);
  }
  ~numa_affinity() {}
  
  mysql_server_numa_infos *get_numa_infos(void) { return m_numa_infos; }

  /**
    check if numa informations is initilized successfully.
    @return true if system and process numa infos have inited successfully.
            false otherwise.
   */
  bool is_numa_inited(void) { return m_numa_infos->is_inited(); }

private:
  /* Point to global numa infos manager. */
  mysql_server_numa_infos *m_numa_infos = nullptr;
};

#endif