# Capstone Project – Team 4

## Project Description

This repository contains the files and code for Team 4's Computer Science Capstone Project at Penn State Abington.

Our project focuses on developing an automated aluminum can inspection system using KEYENCE cameras, sensors, and machine vision technology to detect defects in aluminum cans.

## 1. Clone the Repository (First Time Only)

1. Open Terminal (Mac) or Git Bash/PowerShell (Windows).
2. Navigate to the folder where you want to save the project.
3. Run:

   ```bash
   git clone https://github.com/hanametwoali711/Capstone-project-team4.git
   ```

4. Open the cloned folder in your preferred code editor.

## 2. Pull Changes (Before Editing)

Open a terminal in the project folder and run:

```bash
git pull origin main
```

This downloads and integrates the latest changes made by your teammates.

**Important:** Always pull the latest changes before starting your work to reduce the chance of conflicts.

## 3. Edit Files

Make your changes in any code editor and save the files.

## 4. Check, Commit, and Push Changes (After Editing)

**Step 1: Check the status of your files**

```bash
git status
```

This shows which files have been modified, added, deleted, or staged for commit.

**Step 2: Stage your changes**

To stage all changes:

```bash
git add .
```

Or, to stage only a specific file:

```bash
git add filename.html
```

Replace `filename.html` with the actual file name. You can also specify multiple files:

```bash
git add index.html README.md
```

**Step 3: Verify your staged changes**

```bash
git status
```

Check that only the files you intend to commit are staged.

If you accidentally staged a file, you can unstage it using:

```bash
git restore --staged filename.html
```

**Step 4: Commit your changes**

```bash
git commit -m "Describe your changes"
```

Replace `"Describe your changes"` with a short, meaningful description of what you updated.

**Step 5: Push your changes to GitHub**

```bash
git push
```

If your local branch is already connected to the remote branch, this command is sufficient.

Alternatively, you can explicitly specify the remote repository and branch:

```bash
git push origin main
```

Both commands will push to the same branch when your current branch is `main` and is configured to track `origin/main`.

If Git reports that no upstream branch is configured, run:

```bash
git push -u origin main
```

After that, you can normally use `git push` for future updates.

**Step 6: Confirm your changes**

Run:

```bash
git status
```

If everything has been committed and pushed successfully, Git should report a clean working tree. You can also check GitHub to confirm your changes appear in the repository.

## Important Notes

- Always pull the latest changes before starting work.
- Save your files before committing.
- Use `git status` to review your changes.
- Use `git add .` to stage all changes, or specify individual filenames to stage only selected files.
- Write clear commit messages explaining what you updated.
- Communicate with teammates when editing the same files.
- If Git reports a conflict or rejects a push, resolve the issue before trying again.
- Make sure you have permission to push to the repository.
