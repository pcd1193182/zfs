use std::collections::HashMap;
use std::{fmt::Display, iter};
use std::fmt::Debug;
use either::Either;
use itertools::Itertools;
use text_io::read;

pub(crate) struct PromptOption {
    pub(crate) prompt_char: &'static str,
    pub(crate) description: &'static str,
}

impl Display for PromptOption {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{0}: {1},", self.prompt_char, self.description)
    }
}

pub(crate) fn print_commands<T>(prompt_map: &HashMap<T, PromptOption>) {
    for po in prompt_map.values() {
        println!("{po}");
    }
    println!("p: Print current configuration");
    println!("h: Display this help message");
}

type SelfFunc<T> = fn(T) -> T;

pub(crate) fn prompt_for_input<T: Copy>(prompt_map: HashMap<T, PromptOption>) -> T {
    let prompt = iter::once("h").chain(iter::once("p").chain(prompt_map.values().map(|p| p.prompt_char))).chain(iter::once("b")).join("");
    loop {
        print!("{prompt} >");
        let input: String = read!();
        for (idx, po) in prompt_map.iter() {
            if input == po.prompt_char {
                return *idx;
            }
        }
         if input == "h".to_string() {
            print_commands(&prompt_map);
        } else {
            println!("invalid option: {input}");
        }
    }
}