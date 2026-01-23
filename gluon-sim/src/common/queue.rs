use std::collections::VecDeque;

#[derive(Debug, Default, Clone)]
pub struct Queue<T>
where
    T: Default,
{
    data: VecDeque<T>,
    cap: usize,
}

impl<T: Default> Queue<T> {
    pub fn new(cap: usize) -> Self {
        Queue {
            data: VecDeque::with_capacity(cap),
            cap: cap,
        }
    }

    pub fn full(&self) -> bool {
        self.data.len() == self.cap
    }

    pub fn empty(&self) -> bool {
        self.data.len() == 0
    }

    pub fn push(&mut self, item: T) -> bool {
        if self.full() {
            false
        } else {
            self.data.push_back(item);
            true
        }
    }

    pub fn pop(&mut self) -> Option<T> {
        self.data.pop_front()
    }

    pub fn peek(&self) -> Option<&T> {
        self.data.front()
    }

    pub fn get_mut(&mut self, idx: usize) -> Option<&mut T> {
        self.data.get_mut(idx)
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }
}
