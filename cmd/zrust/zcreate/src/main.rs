use std::{error::Error, fmt::{Debug, Display}, path::Path, sync::OnceLock};
use nvpair::NvList;
use config::Config;

use clap::Parser;

mod vdev;
use pool::Repl;
use vdev::Vdev;

mod prompt;
mod pool;

static VDEV_TUNINGS : OnceLock<Config> = OnceLock::new();

#[derive(Parser, Debug)]
#[command(version, about, long_about = None)]
struct Args {
    /// Disable all featureflags
    #[arg(short='d', long)]
    disable_features: bool,

    /// Force use of vdevs
    #[arg(short='f', long)]
    force: bool,

    /// Dry-run, don't actually create the pool
    #[arg(short='n', long)]
    dryrun: bool,

    /// Pool options
    #[arg(short='o', long)]
    options: Vec<String>,

    /// FS options for the root dataset
    #[arg(short='O', long)]
    fs_options: Vec<String>,

    /// Tempname
    #[arg(short='t', long)]
    tname: Option<String>,

    #[arg(long, default_value="/etc/zfs/zcreate.toml")]
    config_file: String,

    poolname: String,
}

fn parse_config(config_file: String) -> Result<Config, Box<dyn Error>> {
    Ok(Config::builder()
        .add_source(config::File::with_name(
            config_file
                .as_str()),
        )
        .build()?)
}

#[derive(Debug)]
struct ParseError{
    message: String
}

impl Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        todo!()
    }
}

impl Error for ParseError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        None
    }

    fn description(&self) -> &str {
        "description() is deprecated; use Display"
    }

    fn cause(&self) -> Option<&dyn Error> {
        self.source()
    }
}

fn parse_props(props: Vec<String>) -> Result<NvList, ParseError> {
    let mut nvl = NvList::new_unique_names();
    for entry in props.iter().rev() {
        let split = entry.split("=").collect::<Vec<&str>>();
        if split.len() != 2 {
            return Err(ParseError {message: format!("invalid prop string: {entry}")});
        }
        let name = split[0];
        let value = split[1];
        if nvl.exists(name) {
            continue;
        }
        nvl.insert(name, value).unwrap();
    }
    Ok(nvl)
}

fn main() {
    let args = Args::parse();
    println!("Hello, world! {args:?}");

    VDEV_TUNINGS.set(parse_config(args.config_file).expect("Could not open config file")).unwrap();

    let fsprops = parse_props(args.fs_options).expect("Could not parse fs_options");
    let props = parse_props(args.options).expect("Could not parse pool options");

    let pool_builder = Repl::new(args.poolname).construct();

    if (args.dryrun) {
        println!("Dry run, final configuration:");
        println!("Config: {pool_builder:?}");
        println!("Pool properties; {props:?}");
        println!("FS properties: {fsprops:?}");
        return;
    }

    let pool = pool_builder.build();
    println!("Config: {pool:?}");

    // zpool_create(g_zfs, poolname, pool.config, props, fsprops)
}