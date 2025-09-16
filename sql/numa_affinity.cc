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

#include "mysqld_error.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/psi/mysql_file.h"
#include "my_sys.h"
#include "numa_affinity.h"
#include "sql/mysqld.h"
#include <stdio.h>
#include <fcntl.h>
#include <sys/syscall.h>

mysql_server_numa_infos m_server_numa_infos;

void mysql_server_numa_infos::cpu_set_or(cpu_set_t *dest, cpu_set_t *src1,  cpu_set_t *src2) {
  for (int cpu_idx = 0; cpu_idx < CPU_SETSIZE; cpu_idx++) {
    if (CPU_ISSET(cpu_idx, src1) || CPU_ISSET(cpu_idx, src2)) {
      CPU_SET(cpu_idx, dest);
    }
  }
}

int mysql_server_numa_infos::node_and_digits(const struct dirent *dptr) {
  char *p = (char *)(dptr->d_name);
  if (*p++ != 'n') return 0;
  if (*p++ != 'o') return 0;
  if (*p++ != 'd') return 0;
  if (*p++ != 'e') return 0;
  do {
    if (!isdigit(*p++))
      return 0;
  } while (*p != '\0');
  return 1;
}

int mysql_server_numa_infos::add_ids_to_list_from_str(cpu_set_t *list, uchar *ids_str) {
  uchar *ids = ids_str;
  int in_range = 0;
  int next_id = 0;
  if ((ids == NULL) || (strlen((const char*)ids) == 0)) {
    return -1;
  }
  for (;;) {
    /* skip over non-digits */
    while (!isdigit(*ids)) {
      if ((*ids == '\n') || (*ids == '\0')) {
        return 0;
      }
      if (*ids++ == '-') {
        in_range = 1;
      }
    }
    int id;
    NUMA_AFFINITY_CONVERT_DIGITS_TO_NUM(ids, id);
    if (!in_range) {
      next_id = id;
    }
    for (; (next_id <= id); next_id++) {
      CPU_SET(next_id, list);
    }
    in_range = 0;
  }
  return 0;
}

/* Get numa nodes information of the machine. */
int mysql_server_numa_infos::get_nodes_info(void) {
  char fname[FN_REFLEN];
  uchar *buf = (uchar *)malloc(NUMA_AFFINITY_BIG_BUF_SIZE);
  if (buf == nullptr) {
    LogErr(ERROR_LEVEL, ER_NUMA_AWARE, "Fail to allocate memory for reading system node files.");
    return 1;
  }

  struct dirent **namelist;
  int num_files = scandir("/sys/devices/system/node", &namelist, node_and_digits, NULL);
  if (num_files < 1) {
    LogErr(ERROR_LEVEL, ER_FIND_NUMA_NODE);
    free(buf);
    return 1;
  }

  m_nodes_num = num_files;
  for (int node_ix = 0; (node_ix < m_nodes_num); node_ix++) {
    int node_id;
    char *p = &namelist[node_ix]->d_name[4];
    NUMA_AFFINITY_CONVERT_DIGITS_TO_NUM(p, node_id);
    if (node_id >= m_nodes_num) {
      LogErr(ERROR_LEVEL, ER_NUMA_AWARE, "Node id is larger than the number of node.");
      free(buf);
      return 1;
    }

    free(namelist[node_ix]);

    snprintf(fname, FN_REFLEN, "/sys/devices/system/node/node%d/cpulist", node_id);
    int fd = mysql_file_open(key_file_nodes_cpulist, fname, O_RDONLY, MYF(0));
    if ((fd >= 0) && (mysql_file_read(fd, buf, NUMA_AFFINITY_BIG_BUF_SIZE, MYF(0)) > 0)) {
      buf[NUMA_AFFINITY_BIG_BUF_SIZE - 1] = '\0';
      /* get cpulist from the cpulist string */
      CPU_ZERO(&(m_nodes_data[node_id].cpu_set));
      if (add_ids_to_list_from_str(&(m_nodes_data[node_id].cpu_set), buf)) {
        LogErr(ERROR_LEVEL, ER_NUMA_AWARE, "There is no cpu listed in cpulist.");
        free(buf);
        return 1;
      }

      my_close(fd, MYF(0));
    } else {
      LogErr(ERROR_LEVEL, ER_NUMA_AWARE, "Could not get node cpu list.");
      free(buf);
      return 1;
    }

    snprintf(fname, FN_REFLEN, "/sys/devices/system/node/node%d/distance", node_id);
    fd = mysql_file_open(key_file_nodes_distance, fname, O_RDONLY, MYF(0));
    if ((fd >= 0) && (mysql_file_read(fd, buf, NUMA_AFFINITY_BIG_BUF_SIZE, MYF(0)) > 0)) {
      for (uchar *p_buf = buf;  (*p_buf != '\n'); ) {
          int lat;
          NUMA_AFFINITY_CONVERT_DIGITS_TO_NUM(p_buf, lat);
          m_nodes_data[node_id].distance.push_back(lat);
          while (*p_buf == ' ') { p_buf++; }
      }

      my_close(fd, MYF(0));
    } else {
      LogErr(ERROR_LEVEL, ER_NUMA_AWARE, "Could not get node distance data.");
      free(buf);
      return 1;
    }
  }

  free(buf);
  return 0;
}

/* Get the information for mysql server process, including number of cpu per numa node, number of numa node
   that can be used to schedule mysql server process. */
int mysql_server_numa_infos::get_process_sched_info(void) {
  CPU_ZERO(&m_process_cpuset);
  pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &m_process_cpuset);

  for (int i = 0; i < m_nodes_num; i++) {
    CPU_ZERO(&(m_process_nodes_data[i].cpu_set));
    m_process_nodes_data[i].cpu_num = 0;
  }

  for (int i = 0; i < CPU_SETSIZE; i++) {
    for (int j = 0; j < m_nodes_num; j++) {
      if (CPU_ISSET(i, &(m_nodes_data[j].cpu_set))) {
        CPU_SET(i, &(m_process_nodes_data[j].cpu_set));
        m_process_nodes_data[j].cpu_num++;
        break;
      }
    }
  }

  int cpu_num_max = m_process_nodes_data[0].cpu_num;
  m_process_rich_node_idx = 0;
  m_process_nodes_num++;
  for (int i = 1; i < m_nodes_num; i++) {
    if (cpu_num_max < m_process_nodes_data[i].cpu_num) {
      cpu_num_max = m_process_nodes_data[i].cpu_num;
      m_process_rich_node_idx = i;
    }

    if (m_process_nodes_data[i].cpu_num > 0) {
      m_process_nodes_num++;
    }
  }

  return 0;
}

void mysql_server_numa_infos::update_affinity_cpuset(int node_num, cpu_set_t *cpuset_affinity) {
  if (node_num > m_process_nodes_num) {
    LogErr(ERROR_LEVEL, ER_NUMA_NODE_NUM_OVERFLOW, node_num, m_process_nodes_num);
    return;
  }

  std::vector<int> rich_node_distance(m_nodes_data[m_process_rich_node_idx].distance);
  uint distance_num = rich_node_distance.size();
  uint distance_cnt = distance_num;
  /* Store the result of sorting distance ascendingly. */
  std::vector<int> node_distance_ordered;
  while (distance_cnt--) {
    int nearest_distance = 0;
    int nearest_node_id;
    /* Find the first valid distance and its node id. */
    for (uint i = 0; i < distance_num; i++) {
      /* Distance equals to 0 means this node has been handled. */
      if (rich_node_distance[i] == 0) {
        continue;
      } else {
        nearest_distance = rich_node_distance[i];
        nearest_node_id = i;
        break;
      }
    }

    for (uint i = 0; i < distance_num; i++) {
      if (rich_node_distance[i] == 0) {
        continue;
      }
      if (nearest_distance > rich_node_distance[i]) {
        nearest_distance = rich_node_distance[i];
        nearest_node_id = i;
      }
    }

    node_distance_ordered.push_back(nearest_node_id);
    /* Set the nearest node distance to 0, so it won't be selected in next rounds. */
    rich_node_distance[nearest_node_id] = 0;
  }

  /* Pick the rich node firstly, then pick the nearest node from the rich node. And so on. */
  int i = 0;
  int num = node_num;
  CPU_ZERO(cpuset_affinity);
  while (num--) {
    cpu_set_or(cpuset_affinity, cpuset_affinity,
      &(m_process_nodes_data[node_distance_ordered[i++]].cpu_set));
  }
}

/**
  Set numa affinity for the caller.
*/
void mysql_server_numa_infos::set_numa_affinity(cpu_set_t *cpuset_affinity) {
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), cpuset_affinity) != 0) {
    LogErr(WARNING_LEVEL, ER_SET_AFFINITY, syscall(SYS_gettid));
  }
}

/**
  Unset numa affinity of the caller.
*/
void mysql_server_numa_infos::unset_numa_affinity(void) {
  if (pthread_setaffinity_np(pthread_self(), sizeof(m_process_cpuset),
    &m_process_cpuset) != 0) {
    LogErr(WARNING_LEVEL, ER_UNSET_AFFINITY, syscall(SYS_gettid));
  }
}