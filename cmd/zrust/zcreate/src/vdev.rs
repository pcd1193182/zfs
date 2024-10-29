use std::fmt::Display;
use std::fmt::Debug;
use std::num::ParseIntError;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering::Relaxed;
use core::num::IntErrorKind;

use text_io::read;

use crate::pool::Repl;


static ID: AtomicU64 = AtomicU64::new(1);
pub trait Vdev: Display + Debug {
    fn show(&self, repl: &Repl, current: Option<u64>, indent: usize);
    fn id(&self) -> u64;
    fn parent(&self) -> u64;
}

#[derive(Clone, Copy)]
enum ParityCount {
    One = 1,
    Two = 2,
    Three = 3,
}

impl Display for ParityCount {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{0}", *self as isize)
    }
}

impl TryFrom<u8> for ParityCount {
    type Error = ParseError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::One),
            2 => Ok(Self::Two),
            3 => Ok(Self::Three),
            n => Err(ParseError {
                parent: None,
                message: Some(format!("invalid parity: {n}"))
            })
        }
    }
}

#[derive(Debug)]
struct ParseError {
    parent: Option<ParseIntError>,
    message: Option<String>,
}

impl From<ParseIntError> for ParseError {
    fn from(value: ParseIntError) -> Self {
        Self {
            parent: Some(value),
            message: None,
        }
    }
}

enum VdevTypes {
    STRIPE,
    LEAF,
    MIRROR(u8),
    RAIDZ(ParityCount),
    DRAID(ParityCount), // TODO technically this should be like DRAID(N, P, C, S)
}

fn get_count(input: String) -> Result<u8, ParseIntError> {
    let tokens = input.split(" ").collect::<Vec<_>>();
    tokens.get(1).unwrap_or(&"").parse::<u8>()
}

impl TryFrom<String> for VdevTypes {
    type Error = ParseError;

    fn try_from(input: String) -> Result<Self, Self::Error> {
        Ok(match input.get(..1).unwrap() {
            "s" => VdevTypes::STRIPE,
            "l" => VdevTypes::LEAF,
            "m" => {
                let way = get_count(input)?;
                VdevTypes::MIRROR(way)
            },
            "z" => {
                let way = get_count(input)?;
                VdevTypes::RAIDZ(ParityCount::try_from(way)?)
            },
            "d" => {
                let way = get_count(input)?;
                VdevTypes::DRAID(ParityCount::try_from(way)?)
            },
            i => Err(ParseError {
                parent: None,
                message: Some(format!("Invalid input {i}")),
            })?
        })
    }
}

impl VdevTypes {
    fn prompt_char(&self) -> String {
        match self {
            VdevTypes::STRIPE => "s",
            VdevTypes::LEAF => "l",
            VdevTypes::MIRROR(_) => "m",
            VdevTypes::RAIDZ(_) => "z",
            VdevTypes::DRAID(_) => "d",
        }.to_string()
    }

    fn description(&self) -> String {
        match self {
            VdevTypes::STRIPE => "Stripe vdev".to_string(),
            VdevTypes::LEAF => "Leaf vdev".to_string(),
            VdevTypes::MIRROR(n) => format!("{n}-way mirror vdev"),
            VdevTypes::RAIDZ(n) => format!("Raidz vdev w/ {n} parity"),
            VdevTypes::DRAID(n) => format!("DRAID vdev w/ {n} parity"),
        }
    }
}

#[derive(Debug)] 
pub(crate) struct StripeVdev {
    id: u64,
    parent: u64,
    children: Vec<u64>
}

impl StripeVdev {
    pub(crate) fn new(parent: u64) -> Self {
        StripeVdev {
            id: ID.fetch_add(1, Relaxed),
            parent: parent,
            children: vec![]
        }
    }
}

impl Display for StripeVdev {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        todo!()
    }
}

impl Vdev for StripeVdev {
    fn show(&self, repl: &Repl, current: Option<u64>, indent: usize) {
        if Some(self.id) == current {
            println!("{} stripe({}):", format!("{:<width$}", "*", width=indent), self.id);
        } else {
            println!("{} stripe({}):", format!("{:<width$}", " ", width=indent), self.id);
        }
        for idx in 1..self.children.len() {
            print!("{idx}: ");
            repl.pool.lookup(self.children[idx]).show(repl, current, indent);
        }
        println!("");
    }
    
    fn id(&self) -> u64 {
        self.id
    }
    
    fn parent(&self) -> u64 {
        self.parent
    }
}

pub(crate) fn select_vdev(parent: u64) -> Box<dyn Vdev> {
    println!("Select vdev type: [s]tripe, [l]eaf, [m]irror <count>, raid[z] <parity>, [d]raid <parity>");
    loop {
        print!("slmzd> ");
        let response: String = read!();
        match VdevTypes::try_from(response) {
            Ok(vdev_type) => match vdev_type{
                VdevTypes::STRIPE => return Box::new(StripeVdev::new(parent)),
                VdevTypes::LEAF => todo!(),
                VdevTypes::MIRROR(n) => todo!(),
                VdevTypes::RAIDZ(parity_count) => todo!(),
                VdevTypes::DRAID(parity_count) => todo!(),
            },
            Err(e) => println!("{e:?}"),
        }
    }
    
}