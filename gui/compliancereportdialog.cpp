#include "compliancereportdialog.h"
#include "ui_compliancereportdialog.h"

#include "filelist.h"
#include "projectfile.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>
#include <QTextStream>

static void addHeaders(const QString& file1, QSet<QString> &allFiles) {
    if (allFiles.contains(file1))
        return;
    QFile file(file1);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    allFiles << file1;
    const QRegularExpression re("^#include[ ]*\"([^\">]+)\".*");
    QTextStream in(&file);
    QString line = in.readLine();
    while (!in.atEnd()) {
        if (line.startsWith("#include")) {
            const QRegularExpressionMatch match = re.match(line);
            if (match.hasMatch()) {
                QString hfile = match.captured(1);
                if (file1.contains("/"))
                    hfile = file1.mid(0,file1.lastIndexOf("/") + 1) + hfile;
                addHeaders(hfile, allFiles);
            }
        }
        line = in.readLine();
    }
}

static std::vector<std::string> toStdStringList(const QStringList& from) {
    std::vector<std::string> ret;
    std::transform(from.cbegin(), from.cend(), std::back_inserter(ret), [](const QString& e) {
        return e.toStdString();
    });
    return ret;
}

ComplianceReportDialog::ComplianceReportDialog(ProjectFile* projectFile, QString resultsFile) :
    QDialog(nullptr),
    mUI(new Ui::ComplianceReportDialog),
    mProjectFile(projectFile),
    mResultsFile(std::move(resultsFile))
{
    mUI->setupUi(this);
    mUI->mEditProjectName->setText(projectFile->getProjectName());
    connect(mUI->buttonBox, &QDialogButtonBox::clicked, this, &ComplianceReportDialog::buttonClicked);
}

ComplianceReportDialog::~ComplianceReportDialog()
{
    delete mUI;
}

void ComplianceReportDialog::buttonClicked(QAbstractButton* button)
{
    switch (mUI->buttonBox->standardButton(button)) {
    case QDialogButtonBox::StandardButton::Save:
        save();
        break;
    case QDialogButtonBox::StandardButton::Close:
        close();
        break;
    default:
        break;
    };
}

void ComplianceReportDialog::save()
{
    const QString outFile = QFileDialog::getSaveFileName(this,
                                                         tr("Compliance report"),
                                                         QDir::homePath() + "/misra-c-2012-compliance-report.html",
                                                         tr("HTML files (*.html)"));
    if (outFile.isEmpty())
        return;

    close();

    const QString& projectName = mUI->mEditProjectName->text();
    const QString& projectVersion = mUI->mEditProjectVersion->text();
    const bool files = mUI->mCheckFiles->isChecked();

    if (projectName != mProjectFile->getProjectName()) {
        mProjectFile->setProjectName(projectName);
        mProjectFile->write();
    }

    QTemporaryFile tempFiles;
    if (files && tempFiles.open()) {
        QTextStream out(&tempFiles);
        FileList fileList;
        fileList.addPathList(mProjectFile->getCheckPaths());
        if (!mProjectFile->getImportProject().isEmpty()) {
            QFileInfo inf(mProjectFile->getFilename());

            QString prjfile;
            if (QFileInfo(mProjectFile->getImportProject()).isAbsolute())
                prjfile = mProjectFile->getImportProject();
            else
                prjfile = inf.canonicalPath() + '/' + mProjectFile->getImportProject();

            ImportProject p;
            try {
                p.import(prjfile.toStdString());
            } catch (InternalError &e) {
                QMessageBox msg(QMessageBox::Critical,
                                tr("Save compliance report"),
                                tr("Failed to import '%1', can not show files in compliance report").arg(prjfile),
                                QMessageBox::Ok,
                                this);
                msg.exec();
                return;
            }

            p.ignorePaths(toStdStringList(mProjectFile->getExcludedPaths()));

            QDir dir(inf.absoluteDir());
            for (const ImportProject::FileSettings& fs: p.fileSettings)
                fileList.addFile(dir.relativeFilePath(QString::fromStdString(fs.filename)));
        }

        QSet<QString> allFiles;
        for (const QString &sourcefile: fileList.getFileList())
            addHeaders(sourcefile, allFiles);
        for (const QString& fileName: allFiles) {
            QFile f(fileName);
            if (f.open(QFile::ReadOnly)) {
                QCryptographicHash hash(QCryptographicHash::Algorithm::Md5);
                if (hash.addData(&f)) {
                    for (auto b: hash.result())
                        out << QString::number((unsigned char)b,16);
                    out << " " << fileName << "\n";
                }
            }
        }
        tempFiles.close();
    }

    QStringList args{"--compliant=misra-c2012-1.1",
                     "--compliant=misra-c2012-1.2",
                     "--project-name=" + projectName,
                     "--project-version=" + projectVersion,
                     "--output-file=" + outFile};
    if (files)
        args << "--files=" + tempFiles.fileName();
    args << mResultsFile;

    const QString appPath = QFileInfo(QCoreApplication::applicationFilePath()).canonicalPath();

    QProcess process;
#ifdef Q_OS_WIN
    process.start(appPath + "/compliance-report.exe", args);
#else
    process.start(appPath + "/compliance-report", args);
#endif
    process.waitForFinished();
}
