// SFTP browser permissions dialog: checkboxes and numeric mode kept in step, special bits kept.
#include <QtWidgets> // icons.h assumes QMap is already included
#include "sftp-panel.h"
#include <cstdio>

static int failures = 0;
static void check(bool ok, const QByteArray &what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.constData());
    failures += !ok;
}

// Runs the dialog with start, lets act change it, then presses OK (or Cancel) and returns the result.
static QString runDialog(const QString &start, const std::function<void(QDialog *)> &act, bool accept = true)
{
    QTimer::singleShot(0,
                       [&]
                       {
                           auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                           if (!dialog)
                               return;
                           act(dialog);
                           accept ? dialog->accept() : dialog->reject();
                       });
    const auto chosen = sftpPermissionsDialog(nullptr, "/srv/www/index.php", start);
    return chosen ? chosen->mode : QString();
}

static void press(QWidget *widget, int key)
{
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier), up(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &down);
    QApplication::sendEvent(widget, &up);
    QCoreApplication::processEvents();
}

static QList<QCheckBox *> boxes(QDialog *dialog) { return dialog->findChildren<QCheckBox *>(); }

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // The dialog shows the starting mode on its checkboxes.
    QString seen;
    check(runDialog("754",
                    [&](QDialog *d)
                    {
                        for (auto *b : boxes(d))
                            seen += b->isChecked() ? '1' : '0';
                        if (argc > 1) // a picture of the dialog for a visual check
                            d->grab().save(argv[1]);
                    }) == "754",
          "unchanged mode returned as it was");
    check(seen == "111101100", "754 on the checkboxes: " + seen.toUtf8());
    // Checking group write and others execute: 754 -> 775.
    check(runDialog("754",
                    [](QDialog *d)
                    {
                        const auto b = boxes(d);
                        b[4]->setChecked(true); // group write
                        b[8]->setChecked(true); // others execute
                        b[6]->setChecked(true); // others read (already)
                    }) == "775",
          "checkboxes change the numeric mode");
    // setuid kept when a checkbox changes: 4755 -> 4750.
    check(runDialog("4755", [](QDialog *d) { boxes(d)[6]->setChecked(false), boxes(d)[8]->setChecked(false); }) == "4750",
          "special digit kept");
    // Typing the numeric value moves the checkboxes.
    QString typed;
    check(runDialog("644",
                    [&](QDialog *d)
                    {
                        d->findChild<QLineEdit *>()->setText("600");
                        for (auto *b : boxes(d))
                            typed += b->isChecked() ? '1' : '0';
                    }) == "600",
          "typed mode returned");
    check(typed == "110000000", "600 on the checkboxes: " + typed.toUtf8());
    // An incomplete value disables OK.
    check(runDialog("644",
                    [](QDialog *d)
                    {
                        d->findChild<QLineEdit *>()->setText("6");
                        check(!d->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled(),
                              "OK disabled for an incomplete mode");
                    })
              .isEmpty(),
          "incomplete mode not returned");
    check(runDialog("644", [](QDialog *) {}, false).isEmpty(), "Cancel returns nothing");
    // With a shell: owner and group, and the folder's content.
    {
        QTimer::singleShot(0,
                           [&]
                           {
                               auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                               if (!d)
                                   return;
                               const auto edits = d->findChildren<QLineEdit *>();
                               check(edits.size() == 3 && edits[1]->text() == "root" && edits[2]->text() == "www",
                                     "owner and group shown");
                               edits[1]->setText("deploy");
                               auto *recurse = d->findChildren<QCheckBox *>().value(9);
                               auto *scope = d->findChild<QComboBox *>();
                               check(recurse && scope && !scope->isEnabled(), "content scope enabled with its checkbox only");
                               if (recurse && scope)
                               {
                                   recurse->setChecked(true);
                                   scope->setCurrentIndex(1); // files only
                               }
                               if (argc > 3)
                                   d->grab().save(argv[3]);
                               d->accept();
                           });
        const auto chosen = sftpPermissionsDialog(nullptr, "/srv/www", "755", "root", "www", true, true);
        check(chosen && chosen->mode == "755" && chosen->owner == "deploy" && chosen->group == "www" && chosen->recurse == 2,
              "owner, group and scope returned");
        QTimer::singleShot(0,
                           []
                           {
                               if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                               {
                                   d->findChildren<QLineEdit *>()[1]->setText("bad name!");
                                   d->accept();
                               }
                           });
        const auto unchanged = sftpPermissionsDialog(nullptr, "/srv/www/a.txt", "644", "root", "www", true, false);
        check(unchanged && unchanged->owner == "root" && unchanged->recurse == 0 &&
                  !QString("bad name!").contains(QRegularExpression("^[A-Za-z0-9._-]+$")),
              "an invalid owner keeps the current one");
    }
    // The panel as it opens (not connected): toolbar and filter row, for a visual check.
    if (argc > 4)
    {
        SftpPanel panel(QJsonObject{{"protocol", "sftp"}, {"host", "127.0.0.1"}, {"port", 1}, {"user", "test"}}, "macos");
        panel.resize(300, 420);
        panel.show();
        QCoreApplication::processEvents();
        panel.grab().save(argv[4]);
    }
    // Rename in place: the editor opens on the row with the name before the extension selected; Enter reports the
    // new name and leaves the row as it was (the listing that follows shows the change).
    {
        SftpTree tree;
        tree.setEditTriggers(QAbstractItemView::NoEditTriggers);
        auto *delegate = new SftpRenameDelegate(&tree);
        QString from, to;
        delegate->renamed = [&](const QString &a, const QString &b) { from = a, to = b; };
        tree.setItemDelegateForColumn(0, delegate);
        auto *item = new QTreeWidgetItem(&tree, {"report.final.pdf"});
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        tree.show();
        tree.editItem(item, 0);
        QCoreApplication::processEvents();
        auto *edit = tree.findChild<QLineEdit *>();
        check(edit && edit->selectedText() == "report.final", "name selected without its extension");
        if (edit)
        {
            edit->setText("summary.pdf");
            press(edit, Qt::Key_Return);
        }
        check(from == "report.final.pdf" && to == "summary.pdf", "Enter reports the new name");
        check(item->text(0) == "report.final.pdf", "row unchanged until listed again");
        to.clear();
        tree.editItem(item, 0);
        QCoreApplication::processEvents();
        if (auto *again = tree.findChild<QLineEdit *>())
        {
            again->setText("other.pdf");
            press(again, Qt::Key_Escape);
        }
        check(to.isEmpty(), "Escape renames nothing");
    }
    // Transfer buttons: they wrap instead of widening the panel; the narrowest is the widest button.
    {
        QWidget box;
        auto *flow = new SftpFlowLayout(&box);
        flow->setContentsMargins(0, 0, 0, 0);
        flow->setSpacing(4);
        QList<QPushButton *> buttons;
        for (const char *text : {"Annulla", "Riprendi", "Pulisci"})
            flow->addWidget(buttons.emplace_back(new QPushButton(text)));
        int widest = 0, all = 8;
        for (auto *b : buttons)
            widest = std::max(widest, b->sizeHint().width()), all += b->sizeHint().width();
        check(box.minimumSizeHint().width() <= widest, "minimum width is the widest button");
        box.resize(all, flow->heightForWidth(all));
        box.show();
        QCoreApplication::processEvents();
        check(buttons[0]->y() == buttons[2]->y(), "one line when wide enough");
        const int narrow = widest + 10;
        box.resize(narrow, flow->heightForWidth(narrow));
        QCoreApplication::processEvents();
        check(buttons[2]->y() > buttons[0]->y() && buttons[2]->geometry().right() < narrow, "wraps when narrow");
        if (argc > 2)
            box.grab().save(argv[2]);
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
