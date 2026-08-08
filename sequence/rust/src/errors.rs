use std::error::Error;
use std::fmt;

#[derive(Debug)]
pub struct ExpectedValue<T: fmt::Debug> {
    value: Option<T>,
}

impl<T: fmt::Debug> fmt::Display for ExpectedValue<T> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Unexpected {:?} at", self.value)
    }
}

impl<T: fmt::Debug> Error for ExpectedValue<T> {}

impl<T: fmt::Debug> ExpectedValue<T> {
    pub fn cast(value: Option<T>) -> Result<T, Self> {
        match value {
            Some(v) => Ok(v),
            None => Err(Self { value }),
        }
    }
}
