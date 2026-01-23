use crate::{common::{
    base::{Configurable, Event},
}, glug::stream::StreamConfig};

pub struct Completion {
    pub eq: Vec<Option<Event>>,
}

impl Configurable<StreamConfig> for Completion {
    fn new(config: &StreamConfig) -> Self {
        Completion {
            eq: vec![None; config.num_sq],
        }
    }
}

impl Completion {

    pub fn set_completion(&mut self, event: Event) {
        let evnt = self.eq.get_mut(event.sid() as usize).expect("sid out of bounds");
        assert!(evnt.is_none(), "impossible unset completion for sid {}", event.sid());
        *evnt = Some(event);
    }

    pub fn try_clear_completion(&mut self) -> Option<Event> {
        self.eq.iter_mut()
        .find(|event| event.is_some())
        .map(|event| {
            let evnt = event.take().expect("impossible");
            *event = None;
            evnt
        })
    }
}
