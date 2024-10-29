use std::{collections::HashMap, fmt::Display, path::Path};

use nvpair::NvList;

use crate::{prompt::{prompt_for_input, PromptOption}, vdev::{self, StripeVdev, Vdev}};

#[derive(Debug)]
pub(crate) struct Pool {
    name: String,
    vdevs: HashMap<u64, Box<dyn Vdev>>,
    data: u64,
    log: Option<u64>,
    cache: Option<u64>,
    dedup: Option<u64>,
    special: Option<u64>,
    spares: Vec<Box<Path>>
}

impl Pool {
    pub(crate) fn lookup(&self, id: u64) -> &Box<dyn Vdev> {
        self.vdevs.get(&id).unwrap()
    }

    pub(crate) fn new(name: String) -> Self {
        let root_vdev = StripeVdev::new(0);
        let id = root_vdev.id();
        let mut vdevs: HashMap<u64, Box<dyn Vdev>> = HashMap::new();
        vdevs.insert(id, Box::new(root_vdev));
        Self {
            name,
            vdevs,
            data: id,
            log: None,
            cache: None,
            dedup: None,
            special: None,
            spares: vec![]
        }
    }

    pub(crate) fn build(self) -> NvList {
        todo!()
    }
    
    fn show(&self, f: &mut std::fmt::Formatter<'_>, repl: &Repl, current: Option<u64>, indent: usize) -> Result<(), std::fmt::Error> {
        write!(f, "{}\n", format!("{:<width$} {1}", " ", self.name, width=indent))?;
        self.vdevs.get(&self.data).unwrap().show(f, repl, current, indent + 2)
    }
}

#[derive(Debug, Clone, Copy)]
enum ReplLocation {
    Root,
    Data(u64),
    Log(Option<u64>),
    Cache(Option<u64>),
    Dedup(Option<u64>),
    Special(Option<u64>),
    Spares(Option<u64>),
}

impl Into<u64> for ReplLocation {
    fn into(self) -> u64 {
        match self {
            ReplLocation::Data(i) => i,
            ReplLocation::Log(Some(i)) => i,
            ReplLocation::Cache(Some(i)) => i,
            ReplLocation::Dedup(Some(i)) => i,
            ReplLocation::Special(Some(i)) => i,
            ReplLocation::Spares(Some(i)) => i,
            _ => panic!("Tried to u64ify {self:?}"),
        }
    }
}
impl ReplLocation {
    fn new_id(&self, id: u64) -> Self {
        match self {
            ReplLocation::Root => panic!("Trying to assign new id to root"),
            ReplLocation::Data(_) => ReplLocation::Data(id),
            ReplLocation::Log(_) => ReplLocation::Log(Some(id)),
            ReplLocation::Cache(_) => ReplLocation::Cache(Some(id)),
            ReplLocation::Dedup(_) => ReplLocation::Dedup(Some(id)),
            ReplLocation::Special(_) => ReplLocation::Special(Some(id)),
            ReplLocation::Spares(_) => ReplLocation::Spares(Some(id)),
        }
    }
}

pub(crate) struct Repl {
    pub(crate) pool: Pool,
    current: ReplLocation,
}

impl Repl {
    pub(crate) fn new(poolname: String) -> Self {
        Self { pool: Pool::new(poolname), current: ReplLocation::Root }
    }
    
    pub(crate) fn construct(mut self) -> Pool {
        loop {
            match self.prompt() {
                Some(x) => self.current = x,
                None => return self.pool,
            }
        }
    }

    fn prompt(&mut self) -> Option<ReplLocation> {
        match self.current {
            ReplLocation::Root => self.root_prompt(),
            ReplLocation::Data(id) => self.vdev_prompt(Some(id)),
            ReplLocation::Log(opt) => self.vdev_prompt(opt),
            ReplLocation::Cache(opt) => self.vdev_prompt(opt),
            ReplLocation::Dedup(opt) => self.vdev_prompt(opt),
            ReplLocation::Special(opt) => self.vdev_prompt(opt),
            ReplLocation::Spares(opt) => self.spares_prompt(opt),
        }
    }
    
    fn root_prompt(&mut self) -> Option<ReplLocation> {
        #[derive(PartialEq, Eq, Hash, Clone, Copy)]
        enum RootPrompt {
            DATA,
            LOG,
            CACHE,
            DEDUP,
            SPECIAL,
            HOT,
            COMMIT,
            PRINT,
        }
        let mut prompt_map = HashMap::new();
        prompt_map.insert(RootPrompt::DATA , PromptOption {
            prompt_char: "d",
            description: "Add/modify top-level data vdev",
        });
        
        prompt_map.insert(RootPrompt::LOG , PromptOption {
            prompt_char: "l",
            description: "Add/modify log vdev",
        });
        
        prompt_map.insert(RootPrompt::CACHE , PromptOption {
            prompt_char: "c",
            description: "Add/modify cache vdev",
        });
        
        prompt_map.insert(RootPrompt::DEDUP , PromptOption {
            prompt_char: "D",
            description: "Add/modify dedup vdev",
        });
        
        prompt_map.insert(RootPrompt::SPECIAL , PromptOption {
            prompt_char: "s",
            description: "Add/modify special vdev",
        });
        
        prompt_map.insert(RootPrompt::HOT , PromptOption {
            prompt_char: "H",
            description: "Add/modify hot spares",
        });
        
        prompt_map.insert(RootPrompt::COMMIT , PromptOption {
            prompt_char: "C",
            description: "Finalize pool configuration",
        });
        prompt_map.insert(RootPrompt::PRINT , PromptOption {
            prompt_char: "p",
            description: "Print configuration",
        });

        let result = prompt_for_input(prompt_map);
        match result as RootPrompt {
            RootPrompt::DATA => Some(ReplLocation::Data(self.pool.data)),
            RootPrompt::LOG => Some(ReplLocation::Log(self.pool.log)),
            RootPrompt::CACHE => Some(ReplLocation::Cache(self.pool.cache)),
            RootPrompt::DEDUP => Some(ReplLocation::Dedup(self.pool.dedup)),
            RootPrompt::SPECIAL => Some(ReplLocation::Special(self.pool.special)),
            RootPrompt::HOT => todo!(),
            RootPrompt::COMMIT => None,
            RootPrompt::PRINT => {
                println!("{self}");
                Some(self.current)
            },
        }
    }
    
    fn vdev_prompt(&mut self, id: Option<u64>) -> Option<ReplLocation> {
        #[derive(PartialEq, Eq, Hash, Clone, Copy)]
        enum VdevPrompt {
            CREATE,
            DELETE,
            EDIT,
            UP,
            PRINT,
        }
        let mut prompt_map = HashMap::new();
        prompt_map.insert(VdevPrompt::CREATE , PromptOption {
            prompt_char: "c",
            description: "Create vdev",
        });
        
        prompt_map.insert(VdevPrompt::DELETE , PromptOption {
            prompt_char: "d",
            description: "Delete vdev",
        });
        
        prompt_map.insert(VdevPrompt::EDIT , PromptOption {
            prompt_char: "e",
            description: "Edit vdev",
        });
        
        prompt_map.insert(VdevPrompt::UP , PromptOption {
            prompt_char: "u",
            description: "Finish editing this vdev",
        });
        
        prompt_map.insert(VdevPrompt::PRINT , PromptOption {
            prompt_char: "p",
            description: "Print configuration",
        });

        let result = prompt_for_input(prompt_map);
        let current_vdev = self.pool.vdevs.get(&self.current.into());
        Some(match result as VdevPrompt {
            VdevPrompt::CREATE => {
                let new_vdev = vdev::select_vdev(current_vdev.map(|x| x.id()).unwrap_or(0));
                let id = new_vdev.id();
                self.pool.vdevs.insert(id, new_vdev);
                self.current.new_id(id)
            },
            VdevPrompt::DELETE => todo!(),
            VdevPrompt::EDIT => todo!(),
            VdevPrompt::UP => todo!(),
            VdevPrompt::PRINT => {
                println!("{self}");
                self.current
            },
        })
    }
    
    fn spares_prompt(&self, opt: Option<u64>) -> Option<ReplLocation> {
        todo!()
    }
}

impl Display for Repl {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self.current)?;
        self.pool.show(f, &self, None, 1)
    }
}