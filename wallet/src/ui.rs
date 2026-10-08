use crate::{
    backend::Core,
    model::{CHAIN, Snapshot, Wallet, coins, number, safe, text},
    store::Store,
};
use anyhow::Result;
use base64::Engine;
use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use ratatui::{
    DefaultTerminal, Frame,
    layout::{Constraint, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Clear, Paragraph, Tabs, Wrap},
};
use serde_json::{Value, json};
use std::{
    io::Write,
    path::PathBuf,
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};
const CYAN: Color = Color::Rgb(93, 211, 222);
const GOLD: Color = Color::Rgb(242, 192, 112);
const MUTED: Color = Color::Rgb(141, 159, 179);
const BG: Color = Color::Rgb(13, 20, 32);
#[derive(Debug)]
pub enum Action {
    Refresh,
    Open(PathBuf, String, String, bool),
    Review(String, String, String),
    Send(Value),
    Retry(String),
    Backup(PathBuf),
    Contact(String, String),
}
pub struct Update {
    view: Snapshot,
    result: Result<Option<Value>>,
    review: bool,
}
#[derive(Clone, PartialEq)]
pub enum Kind {
    Send,
    Confirm,
    Open(bool),
    Backup,
    Contact,
}
pub struct Form {
    pub kind: Kind,
    fields: Vec<(String, String)>,
    index: usize,
}
pub struct Ui {
    pub view: Snapshot,
    pub tab: usize,
    pub selected: usize,
    pub form: Option<Form>,
    pub review: Option<Value>,
    message: String,
    error: bool,
    busy: bool,
    quitting: bool,
    refresh_due: Instant,
}
impl Ui {
    pub fn new(view: Snapshot) -> Self {
        Self {
            view,
            tab: 0,
            selected: 0,
            form: None,
            review: None,
            message: "Welcome to Constella".into(),
            error: false,
            busy: false,
            quitting: false,
            refresh_due: Instant::now(),
        }
    }
    fn form(&mut self, kind: Kind, fields: Vec<(&str, String)>) {
        self.form = Some(Form {
            kind,
            fields: fields.into_iter().map(|(k, v)| (k.into(), v)).collect(),
            index: 0,
        });
        self.error = false;
    }
    fn can_spend(&mut self) -> bool {
        if self.view.watch || self.view.demo || self.view.address.is_empty() {
            self.message = "Open a spendable wallet to use this action.".into();
            false
        } else {
            true
        }
    }
    fn send_form(&mut self) {
        if self.can_spend() {
            self.form(
                Kind::Send,
                vec![
                    ("Recipient address or contact", String::new()),
                    ("Amount", String::new()),
                    ("Fee", "0.001".into()),
                ],
            );
        }
    }
    fn open_form(&mut self, create: bool) {
        if self.view.watch || self.view.demo {
            self.message = "Watch-only/demo mode does not access keys.".into();
            return;
        }
        self.form(
            Kind::Open(create),
            vec![
                ("Wallet key file", self.view.key.display().to_string()),
                ("Node", self.view.peer.clone()),
                (
                    "Explorer (empty disables history)",
                    self.view.explorer.clone(),
                ),
            ],
        );
    }
    pub fn key(&mut self, key: KeyEvent, size: Rect) -> Result<Option<Action>> {
        if key.kind == KeyEventKind::Release {
            return Ok(None);
        }
        if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
            self.quitting = true;
            return Ok(None);
        }
        if size.width < 78 || size.height < 24 {
            if key.code == KeyCode::Char('q') {
                self.quitting = true;
            }
            if key.code == KeyCode::Esc {
                self.form = None;
                self.review = None;
            }
            return Ok(None);
        }
        if self.busy {
            if key.code == KeyCode::Char('q') {
                self.quitting = true;
                self.message = "Finishing current operation before exiting...".into();
            }
            // Navigation remains responsive while network operations run.
            if self.form.is_none() {
                self.navigate(key.code);
            }
            return Ok(None);
        }
        if self.form.is_some() {
            return self.form_key(key);
        }
        self.navigate(key.code);
        match key.code {
            KeyCode::Char('q') => self.quitting = true,
            KeyCode::Char('r') if !self.view.demo => return Ok(Some(Action::Refresh)),
            KeyCode::Char('S' | 's') => self.send_form(),
            KeyCode::Enter if self.tab == 1 => self.send_form(),
            KeyCode::Char('N' | 'n') => self.open_form(true),
            KeyCode::Char('O' | 'o') => self.open_form(false),
            KeyCode::Char('B' | 'b') if self.can_spend() => {
                self.form(Kind::Backup, vec![("New backup file", String::new())])
            }
            KeyCode::Char('A' | 'a') if !self.view.demo => self.form(
                Kind::Contact,
                vec![
                    ("Contact name", String::new()),
                    ("Public address", String::new()),
                ],
            ),
            KeyCode::Char('C') if !self.view.address.is_empty() => {
                let encoded = base64::engine::general_purpose::STANDARD.encode(&self.view.address);
                print!("\x1b]52;c;{encoded}\x07");
                std::io::stdout().flush()?;
                self.message =
                    "Address copy requested (requires terminal clipboard support).".into();
            }
            KeyCode::Char('R') if self.tab == 3 && self.can_spend() => {
                if let Some(row) = self.view.rows().get(self.selected) {
                    return Ok(Some(Action::Retry(text(row, "id").into())));
                }
            }
            _ => {}
        }
        Ok(None)
    }
    fn navigate(&mut self, key: KeyCode) {
        match key {
            KeyCode::Char(c @ '1'..='4') => self.tab = c as usize - '1' as usize,
            KeyCode::Tab | KeyCode::Right => self.tab = (self.tab + 1) % 4,
            KeyCode::BackTab | KeyCode::Left => self.tab = (self.tab + 3) % 4,
            KeyCode::Down if self.tab == 3 => {
                self.selected = (self.selected + 1).min(self.view.rows().len().saturating_sub(1))
            }
            KeyCode::Up if self.tab == 3 => self.selected = self.selected.saturating_sub(1),
            _ => {}
        }
    }
    fn form_key(&mut self, key: KeyEvent) -> Result<Option<Action>> {
        if key.code == KeyCode::Esc {
            self.form = None;
            self.review = None;
            self.message = "Cancelled.".into();
            return Ok(None);
        }
        let form = self.form.as_mut().unwrap();
        let count = form.fields.len();
        match key.code {
            KeyCode::Tab | KeyCode::Down => form.index = (form.index + 1) % (count + 1),
            KeyCode::BackTab | KeyCode::Up => form.index = (form.index + count) % (count + 1),
            KeyCode::Backspace if form.index < count => {
                form.fields[form.index].1.pop();
            }
            KeyCode::Char('u')
                if key.modifiers.contains(KeyModifiers::CONTROL) && form.index < count =>
            {
                form.fields[form.index].1.clear()
            }
            KeyCode::Char(c)
                if !key
                    .modifiers
                    .intersects(KeyModifiers::CONTROL | KeyModifiers::ALT)
                    && form.index < count
                    && c.is_ascii()
                    && !c.is_ascii_control() =>
            {
                if form.fields[form.index].1.len() < 4096 {
                    form.fields[form.index].1.push(c);
                }
            }
            KeyCode::Enter => {
                if form.index < count && form.kind != Kind::Confirm {
                    form.index += 1;
                    return Ok(None);
                }
                let f: Vec<String> = form.fields.iter().map(|(_, v)| v.clone()).collect();
                let action = match form.kind {
                    Kind::Send => Action::Review(f[0].clone(), f[1].clone(), f[2].clone()),
                    Kind::Confirm => {
                        if f[0] != "SEND" {
                            self.message = "Type SEND exactly to authorize this payment.".into();
                            return Ok(None);
                        }
                        let Some(review) = self.review.clone() else {
                            return Ok(None);
                        };
                        if !self.can_spend() {
                            return Ok(None);
                        }
                        Action::Send(review)
                    }
                    Kind::Open(create) => Action::Open(
                        crate::expand(PathBuf::from(&f[0]))?,
                        f[1].clone(),
                        f[2].clone(),
                        create,
                    ),
                    Kind::Backup => Action::Backup(crate::expand(PathBuf::from(&f[0]))?),
                    Kind::Contact => Action::Contact(f[0].clone(), f[1].clone()),
                };
                return Ok(Some(action));
            }
            _ => {}
        }
        Ok(None)
    }
    fn update(&mut self, u: Update) {
        self.view = u.view;
        self.busy = false;
        self.refresh_due = Instant::now() + Duration::from_secs(15);
        match u.result {
            Ok(Some(r)) if u.review => {
                self.review = Some(r);
                self.form(Kind::Confirm, vec![("Type SEND to confirm", String::new())]);
                self.message = "Review every detail before signing.".into();
            }
            Ok(Some(r)) => {
                self.message = format!(
                    "Payment {}. Check History for inclusion.",
                    text(&r, "status")
                );
                self.form = None;
                self.review = None;
                self.tab = 3;
            }
            Ok(None) => {
                if self.form.is_some() {
                    self.message = "Done.".into();
                    self.form = None;
                } else {
                    self.message = "Balance and history checked.".into();
                }
                self.error = false;
            }
            Err(e) => {
                self.message = format!("{e:#}");
                self.error = true;
                if self.form.as_ref().is_some_and(|f| f.kind == Kind::Confirm) {
                    self.form = None;
                    self.review = None;
                    self.tab = 3;
                }
            }
        }
    }
    pub fn draw(&self, f: &mut Frame) {
        let area = f.area();
        f.render_widget(
            Block::new().style(Style::default().bg(BG).fg(Color::White)),
            area,
        );
        if area.width < 78 || area.height < 24 {
            f.render_widget(Paragraph::new("Constella Wallet\nResize to at least 78 x 24.\nPayment confirmation is disabled here.\nq: quit  Esc: cancel"), area);
            return;
        }
        let parts = Layout::vertical([
            Constraint::Length(3),
            Constraint::Length(3),
            Constraint::Min(1),
            Constraint::Length(5),
        ])
        .margin(1)
        .split(area);
        let mode = if self.view.demo {
            "  DEMO / OFFLINE"
        } else if self.view.watch {
            "  WATCH-ONLY"
        } else {
            ""
        };
        f.render_widget(
            Paragraph::new(Line::from(vec![
                Span::styled(
                    "  C O N S T E L L A",
                    Style::default().fg(CYAN).add_modifier(Modifier::BOLD),
                ),
                Span::styled(
                    format!("    WALLET   /   TESTNET v5{mode}"),
                    Style::default().fg(GOLD),
                ),
            ]))
            .block(
                Block::new()
                    .borders(Borders::BOTTOM)
                    .border_style(Style::default().fg(MUTED)),
            ),
            parts[0],
        );
        f.render_widget(
            Tabs::new(["1 Overview", "2 Send", "3 Receive", "4 History"])
                .select(self.tab)
                .highlight_style(
                    Style::default()
                        .fg(BG)
                        .bg(CYAN)
                        .add_modifier(Modifier::BOLD),
                )
                .style(Style::default().fg(MUTED))
                .divider("   "),
            parts[1],
        );
        let body = Rect {
            x: parts[2].x + 2,
            y: parts[2].y,
            width: parts[2].width.saturating_sub(4),
            height: parts[2].height,
        };
        let mut lines: Vec<Line> = vec![];
        if self.view.address.is_empty() {
            lines.extend([
                Line::styled(
                    "Your keys. Your wallet.",
                    Style::default().fg(CYAN).add_modifier(Modifier::BOLD),
                ),
                Line::from(""),
                Line::from("N  Create a new wallet"),
                Line::from("O  Open an existing wallet or private backup"),
                Line::from(""),
                Line::from("A new wallet starts at zero. Back up its key before funding it."),
                Line::from("Mining wallets stay in their original locations."),
            ]);
        } else {
            match self.tab {
                0 => {
                    lines.push(Line::styled("BALANCE", Style::default().fg(MUTED)));
                    lines.push(Line::styled(
                        self.view
                            .account
                            .as_ref()
                            .map(|a| coins(number(&a["balance"]).unwrap_or(0)))
                            .unwrap_or_else(|| "Connecting...".into()),
                        Style::default().fg(CYAN).add_modifier(Modifier::BOLD),
                    ));
                    lines.push(Line::from("testnet coins"));
                    lines.push(Line::from(""));
                    lines.push(Line::from(format!("Wallet   {}", self.view.address)));
                    lines.push(Line::from(format!(
                        "Outgoing nonce: {}   |   Balance checked {}",
                        self.view
                            .account
                            .as_ref()
                            .map(|a| number(&a["nonce"]).unwrap_or(0).to_string())
                            .unwrap_or_else(|| "--".into()),
                        self.view
                            .account_at
                            .map(|t| format!("{}s ago", t.elapsed().as_secs()))
                            .unwrap_or_else(|| "never".into())
                    )));
                    lines.push(Line::from(""));
                    lines.push(Line::styled("RECENT ACTIVITY", Style::default().fg(GOLD)));
                    for row in self.view.rows().iter().take(3) {
                        lines.push(Line::from(format!(
                            "{:<10} {:>18}   {}",
                            if row["from"] == self.view.address {
                                "Sent"
                            } else {
                                "Received"
                            },
                            coins(number(&row["amount"]).unwrap_or(0)),
                            safe(text(row, "status"))
                        )));
                    }
                    if self.view.rows().is_empty() {
                        lines.push(Line::from(
                            "No recent transfers. Receive coins to get started.",
                        ));
                    }
                }
                1 => {
                    lines.extend([
                        Line::styled("Send testnet coins", Style::default().fg(CYAN)),
                        Line::from(""),
                        Line::from("S or Enter  Compose a payment"),
                        Line::from(""),
                        Line::from("Review recipient, amount, fee and total before typing SEND."),
                        Line::from("Saved payments stay in History until inclusion is verified."),
                        Line::from(""),
                        Line::from("A  Add a contact; use its name as the recipient"),
                    ]);
                    if self.view.watch || self.view.demo {
                        lines.push(Line::styled(
                            "Signing is disabled in this mode.",
                            Style::default().fg(GOLD),
                        ));
                    }
                }
                2 => {
                    lines.extend([
                        Line::styled("Receive testnet coins", Style::default().fg(CYAN)),
                        Line::from(""),
                        Line::from("Share this public address. Keep the key private."),
                        Line::from(""),
                        Line::styled(self.view.address.clone(), Style::default().fg(GOLD)),
                        Line::from(""),
                        Line::from("C  Copy address through your terminal"),
                        Line::from(""),
                        Line::from(format!("Network: testnet v5   Chain: {CHAIN}")),
                    ]);
                }
                _ => {
                    let rows = self.view.rows();
                    let selected = self.selected.min(rows.len().saturating_sub(1));
                    lines.push(Line::styled(
                        "Recent transactions",
                        Style::default().fg(CYAN),
                    ));
                    lines.push(Line::from("Inclusion can change after a reorganization."));
                    lines.push(Line::from(""));
                    if rows.is_empty() {
                        lines.push(Line::from("No recent transactions available."));
                    }
                    let capacity = body.height.saturating_sub(8).max(1) as usize;
                    let start = (selected + 1).saturating_sub(capacity);
                    for (i, row) in rows.iter().enumerate().skip(start).take(capacity) {
                        lines.push(Line::styled(
                            format!(
                                "{} {:<9} {:>18}  {:<14} {}",
                                if i == selected { ">" } else { " " },
                                if row["from"] == self.view.address {
                                    "Sent"
                                } else {
                                    "Received"
                                },
                                coins(number(&row["amount"]).unwrap_or(0)),
                                safe(text(row, "status")),
                                number(&row["height"])
                                    .map(|n| n.to_string())
                                    .unwrap_or_else(|_| "--".into())
                            ),
                            Style::default().fg(if i == selected { CYAN } else { Color::White }),
                        ));
                    }
                    if let Some(row) = rows.get(selected) {
                        lines.push(Line::from(""));
                        lines.push(Line::from(format!("Tx: {}", text(row, "id"))));
                        lines.push(Line::from(format!("To: {}", text(row, "to"))));
                        let depth = if row["status"] == "applied" {
                            number(&row["height"])
                                .map(|h| self.view.chain_height.saturating_sub(h) + 1)
                                .unwrap_or(0)
                        } else {
                            0
                        };
                        lines.push(Line::from(format!(
                            "Fee {} | {depth} share confirmations (Explorer snapshot)",
                            coins(number(&row["fee"]).unwrap_or(0))
                        )));
                    }
                    lines.push(Line::styled(
                        "Up/down: select   R: retry saved payment   r: refresh",
                        Style::default().fg(MUTED),
                    ));
                }
            }
        }
        f.render_widget(Paragraph::new(lines), body);
        let connected = self
            .view
            .account_at
            .is_some_and(|t| t.elapsed() < Duration::from_secs(60))
            && self.view.node_error.is_empty();
        let status = if self.view.demo {
            "demo data (offline)"
        } else if connected {
            "node connected"
        } else {
            "node offline / stale"
        };
        let activity = if self.busy {
            if self.quitting {
                "Finishing current operation before exiting...".into()
            } else {
                format!(
                    "{} Working...",
                    ["|", "/", "-", "\\"][Instant::now()
                        .duration_since(self.refresh_due.min(Instant::now()))
                        .as_millis() as usize
                        / 150
                        % 4]
                )
            }
        } else {
            safe(&self.message)
        };
        let warning = if !self.view.node_error.is_empty() {
            &self.view.node_error
        } else {
            &self.view.history_error
        };
        f.render_widget(
            Paragraph::new(vec![
                Line::styled(safe(warning), Style::default().fg(GOLD)),
                Line::styled(
                    activity,
                    Style::default().fg(if self.error { Color::LightRed } else { CYAN }),
                ),
                Line::styled(
                    if self.form.is_some() {
                        "Tab: fields / action   Enter: continue   Ctrl-U: clear   Esc: cancel"
                    } else {
                        "1-4 / Tab: screens   r: refresh   S: send   C: copy   B: backup   q: quit"
                    },
                    Style::default().fg(MUTED),
                ),
                Line::from(format!(
                    "{status}  |  {}  |  share {}",
                    safe(&self.view.peer),
                    self.view
                        .account
                        .as_ref()
                        .and_then(|a| number(&a["height"]).ok())
                        .unwrap_or(0)
                )),
            ])
            .block(
                Block::new()
                    .borders(Borders::TOP)
                    .border_style(Style::default().fg(MUTED)),
            ),
            parts[3],
        );
        if let Some(form) = &self.form {
            self.draw_form(f, form);
        }
    }
    fn draw_form(&self, f: &mut Frame, form: &Form) {
        let area = f.area();
        let width = area.width.min(100).saturating_sub(4);
        let height = if form.kind == Kind::Confirm {
            20
        } else {
            (form.fields.len() * 3 + 5) as u16
        };
        let rect = Rect::new(
            (area.width - width) / 2,
            (area.height - height) / 2,
            width,
            height,
        );
        f.render_widget(Clear, rect);
        let title = match form.kind {
            Kind::Send => "Compose payment",
            Kind::Confirm => "Review your payment",
            Kind::Open(true) => "Create wallet",
            Kind::Open(false) => "Open wallet / settings",
            Kind::Backup => "Back up wallet",
            Kind::Contact => "Save contact",
        };
        let block = Block::bordered()
            .title(format!(" {title} "))
            .style(Style::default().bg(BG).fg(CYAN));
        let inner = block.inner(rect);
        f.render_widget(block, rect);
        let mut lines = vec![];
        if form.kind == Kind::Confirm
            && let Some(r) = &self.review
        {
            lines.extend([
                Line::from(format!("Testnet v5   Chain {CHAIN}")),
                Line::from("From:"),
                Line::styled(text(r, "from").to_owned(), Style::default().fg(GOLD)),
                Line::from("To:"),
                Line::styled(text(r, "to").to_owned(), Style::default().fg(GOLD)),
                Line::from(format!(
                    "Amount: {}",
                    coins(number(&r["amount"]).unwrap_or(0))
                )),
                Line::from(format!("Fee:    {}", coins(number(&r["fee"]).unwrap_or(0)))),
                Line::from(format!(
                    "Total:  {}",
                    coins(
                        number(&r["amount"])
                            .unwrap_or(0)
                            .saturating_add(number(&r["fee"]).unwrap_or(0))
                    )
                )),
                Line::from(format!("Nonce:  {}", number(&r["nonce"]).unwrap_or(0))),
                Line::from(""),
            ]);
        }
        for (i, (label, value)) in form.fields.iter().enumerate() {
            lines.push(Line::styled(label.clone(), Style::default().fg(MUTED)));
            // Keep the end of long editable fields visible without splitting UTF-8.
            let visible = safe(value);
            let max = inner.width.saturating_sub(4) as usize;
            let visible = &visible[visible.len().saturating_sub(max)..];
            lines.push(Line::styled(
                format!(
                    "{} {}{}",
                    if i == form.index { ">" } else { " " },
                    visible,
                    if i == form.index { "_" } else { "" }
                ),
                Style::default().fg(if i == form.index { GOLD } else { Color::White }),
            ));
            lines.push(Line::from(""));
        }
        lines.push(Line::styled(
            format!(
                "{} [ {} ]    Esc: cancel",
                if form.index == form.fields.len() {
                    ">"
                } else {
                    " "
                },
                if form.kind == Kind::Confirm {
                    "Sign and send"
                } else if form.kind == Kind::Send {
                    "Review payment"
                } else {
                    "Continue"
                }
            ),
            Style::default().fg(GOLD),
        ));
        if self.error {
            lines.push(Line::styled(
                safe(&self.message),
                Style::default().fg(Color::LightRed),
            ));
        }
        f.render_widget(Paragraph::new(lines).wrap(Wrap { trim: false }), inner);
    }
}
fn perform(wallet: &mut Wallet, action: Action) -> Result<Option<Value>> {
    match action {
        Action::Refresh => wallet.refresh(),
        Action::Open(key, peer, explorer, create) => {
            wallet.open(key, peer, explorer, create)?;
            wallet.refresh();
        }
        Action::Review(to, n, fee) => return wallet.review(&to, &n, &fee).map(Some),
        Action::Send(r) => return wallet.send(&r).map(Some),
        Action::Retry(id) => return wallet.retry(&id).map(Some),
        Action::Backup(path) => wallet.backup(&path)?,
        Action::Contact(name, addr) => wallet.contact(&name, &addr)?,
    }
    Ok(None)
}
pub fn run(mut wallet: Wallet) -> Result<()> {
    let mut ui = Ui::new(wallet.snapshot());
    let (jobs, receiver) = mpsc::channel::<Action>();
    let (updates, results) = mpsc::channel();
    let worker = thread::spawn(move || {
        while let Ok(action) = receiver.recv() {
            let review = matches!(action, Action::Review(..));
            let result = perform(&mut wallet, action);
            if updates
                .send(Update {
                    view: wallet.snapshot(),
                    result,
                    review,
                })
                .is_err()
            {
                break;
            }
        }
    });
    let mut terminal = ratatui::init();
    let result = event_loop(&mut terminal, &mut ui, &jobs, &results);
    ratatui::restore();
    drop(jobs);
    // Join even on terminal I/O failure: an in-flight send must finish saving its result.
    let joined = worker.join();
    result?;
    anyhow::ensure!(
        joined.is_ok(),
        "Wallet worker stopped unexpectedly; check saved receipts before retrying"
    );
    Ok(())
}
fn event_loop(
    terminal: &mut DefaultTerminal,
    ui: &mut Ui,
    jobs: &mpsc::Sender<Action>,
    updates: &mpsc::Receiver<Update>,
) -> Result<()> {
    loop {
        match updates.try_recv() {
            Ok(u) => ui.update(u),
            Err(mpsc::TryRecvError::Disconnected) => {
                anyhow::bail!("Wallet worker stopped; check saved receipts")
            }
            Err(mpsc::TryRecvError::Empty) => {}
        }
        if ui.quitting && !ui.busy {
            break;
        }
        if !ui.busy
            && !ui.quitting
            && ui.form.is_none()
            && !ui.view.demo
            && !ui.view.address.is_empty()
            && Instant::now() >= ui.refresh_due
        {
            jobs.send(Action::Refresh)?;
            ui.busy = true;
        }
        terminal.draw(|f| ui.draw(f))?;
        if event::poll(Duration::from_millis(100))?
            && let Event::Key(k) = event::read()?
        {
            let size = terminal.size()?;
            match ui.key(k, Rect::new(0, 0, size.width, size.height)) {
                Ok(Some(a)) => {
                    jobs.send(a)?;
                    ui.busy = true;
                    ui.error = false;
                }
                Ok(None) => {}
                Err(e) => {
                    ui.message = e.to_string();
                    ui.error = true;
                }
            }
        }
    }
    Ok(())
}
struct DemoCore;
impl Core for DemoCore {
    fn call(&self, _: &[&str]) -> Result<Value> {
        anyhow::bail!("Demo does not access a backend")
    }
}
pub fn demo_wallet() -> Wallet {
    let mut w = Wallet::new(
        Box::new(DemoCore),
        Store::memory(),
        PathBuf::from("/demo/wallet.key"),
        "demo.node:18473".into(),
        String::new(),
        true,
    );
    w.view.demo = true;
    w.view.address = "a1".repeat(32);
    w.view.account = Some(json!({"balance":"217218415772","nonce":"1","next":"1","height":39180}));
    w.view.account_at = Some(Instant::now());
    w.view.chain_height = 39180;
    w.view.history = vec![
        json!({"id":"e3".repeat(32),"from":w.view.address,"to":"fc".repeat(32),"amount":100000000,"fee":100000,"nonce":0,"height":39072,"status":"applied"}),
        json!({"id":"66".repeat(32),"from":"fc".repeat(32),"to":w.view.address,"amount":100000000,"fee":100000,"nonce":0,"height":39079,"status":"applied"}),
    ];
    w
}
pub fn demo() -> Result<()> {
    run(demo_wallet())
}
#[cfg(test)]
impl Ui {
    pub fn test_confirm(&mut self, s: &str) {
        self.form(Kind::Confirm, vec![("Type SEND to confirm", s.into())]);
    }
}
