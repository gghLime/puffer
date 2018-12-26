#include <iostream>
#include <string>
#include <sstream>
#include <memory>
#include <crypto++/sha.h>
#include <crypto++/hex.h>
#include <pqxx/pqxx>

#include "yaml.hh"
#include "filesystem.hh"
#include "path.hh"
#include "child_process.hh"
#include "system_runner.hh"
#include "influxdb_client.hh"
#include "util.hh"
#include "exception.hh"
#include "timestamp.hh"

using namespace std;
using namespace CryptoPP;

static string yaml_config;
static YAML::Node config;
static fs::path src_path;  /* path to puffer/src directory */

void print_usage(const string & program_name)
{
  cerr << "Usage: " << program_name << " <YAML configuration>" << endl;
}

string sha256(const string & input)
{
  SHA256 hash;
  string digest;
  StringSource s(input, true,
    new HashFilter(hash,
      new HexEncoder(
        new StringSink(digest))));
  return digest;
}

int retrieve_expt_id(const string & json_str)
{
  try {
    /* compute SHA-256 */
    string hash = sha256(json_str);

    /* connect to PostgreSQL */
    string db_conn_str = postgres_connection_string(config["postgres_connection"]);
    pqxx::connection db_conn(db_conn_str);

    /* create table if not exists */
    pqxx::work create_table(db_conn);
    create_table.exec("CREATE TABLE IF NOT EXISTS puffer_experiment "
                      "(id SERIAL PRIMARY KEY,"
                      " hash VARCHAR(64) UNIQUE NOT NULL,"
                      " data jsonb);");
    create_table.commit();

    /* prepare two statements */
    db_conn.prepare("select_id",
      "SELECT id FROM puffer_experiment WHERE hash = $1;");
    db_conn.prepare("insert_json",
      "INSERT INTO puffer_experiment (hash, data) VALUES ($1, $2) RETURNING id;");

    pqxx::work db_work(db_conn);

    /* try to fetch an existing row */
    pqxx::result r = db_work.prepared("select_id")(hash).exec();
    if (r.size() == 1 and r[0].size() == 1) {
      /* the same hash already exists */
      return r[0][0].as<int>();
    }

    /* insert if no record exists and return the ID of inserted row */
    r = db_work.prepared("insert_json")(hash)(json_str).exec();
    db_work.commit();
    if (r.size() == 1 and r[0].size() == 1) {
      return r[0][0].as<int>();
    }
  } catch (const exception & e) {
    print_exception("retrieve_expt_id", e);
  }

  return -1;
}

int main(int argc, char * argv[])
{
  if (argc < 1) {
    abort();
  }

  if (argc != 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  /* save as global variables */
  yaml_config = fs::absolute(argv[1]);
  config = YAML::LoadFile(yaml_config);
  src_path = fs::canonical(fs::path(
      roost::readlink("/proc/self/exe")).parent_path().parent_path());

  ProcessManager proc_manager;

  const bool enable_logging = config["enable_logging"].as<bool>();
  if (enable_logging) {
    cerr << "Logging is enabled" << endl;
  } else {
    cerr << "Logging is disabled" << endl;
  }

  /* create an influxdb_client only if enable_logging is true */
  unique_ptr<InfluxDBClient> influxdb_client = nullptr;
  if (enable_logging) {
    /* add influx client for posting states of child processes */
    const auto & influx = config["influxdb_connection"];
    influxdb_client = make_unique<InfluxDBClient>(
        proc_manager.poller(),
        Address(influx["host"].as<string>(), influx["port"].as<uint16_t>()),
        influx["dbname"].as<string>(),
        influx["user"].as<string>(),
        safe_getenv(influx["password"].as<string>()));
  }

  /* will run log reporters only if enable_logging is true */
  auto log_reporter = src_path / "monitoring/log_reporter";
  vector<string> log_stems {
    "active_streams", "rebuffer_events", "client_buffer", "client_sysinfo",
    "video_sent", "video_acked"};

  /* run media servers in each experimental group */
  const auto & expt_json = src_path / "scripts/expt_json.py";
  const auto & ws_media_server = src_path / "media-server/ws_media_server";

  int server_id = 0;
  for (const auto & expt : config["experiments"]) {
    /* convert YAML::Node to string */
    stringstream ss;
    ss << expt["fingerprint"];
    string fingerprint = ss.str();

    /* run expt_json.py to represent experimental settings as a JSON string */
    string json_str = run(expt_json, {expt_json, fingerprint}, true).first;

    int expt_id = retrieve_expt_id(json_str);
    unsigned int num_servers = expt["num_servers"].as<unsigned int>();

    cerr << "Running experiment " << expt_id << " on "
         << num_servers << " servers" << endl;

    for (unsigned int i = 0; i < num_servers; i++) {
      server_id++;

      vector<string> args { ws_media_server, yaml_config,
                            to_string(server_id), to_string(expt_id) };
      proc_manager.run_as_child(ws_media_server, args, {},
        [&influxdb_client, server_id](const pid_t &)  // error callback
        {
          cerr << "Error in media server with ID " << server_id << endl;
          if (influxdb_client) {
            /* at least one media server have failed */
            influxdb_client->post("server_state state=1i "
                                  + to_string(timestamp_ms()));
          }
        }
      );

      /* run log_reporters */
      if (influxdb_client) {
        fs::path log_dir = config["log_dir"].as<string>();

        for (const auto & log_stem : log_stems) {
          string log_format = log_dir / (log_stem + ".conf");
          string log_path = log_dir / (log_stem + "." + to_string(server_id)
                                       + ".log");

          vector<string> log_args { log_reporter, yaml_config,
                                    log_format, log_path };
          proc_manager.run_as_child(log_reporter, log_args, {},
            [&influxdb_client, log_stem](const pid_t &)  // error callback
            {
              cerr << "Error in log reporter: " << log_stem << endl;
              /* at least one log reporter have failed */
              influxdb_client->post("log_reporter_state state=1i "
                                    + to_string(timestamp_ms()));
            }
          );
        }
      }
    }
  }

  /* more logging actions */
  if (influxdb_client) {
    /* indicate that media servers are running */
    influxdb_client->post("server_state state=0i "
                          + to_string(timestamp_ms()));

    /* indicate that log reporters are running */
    influxdb_client->post("log_reporter_state state=0i "
                          + to_string(timestamp_ms()));
  }

  return proc_manager.wait();
}
