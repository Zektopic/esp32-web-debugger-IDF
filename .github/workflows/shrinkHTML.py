import os
import htmlmin
import sys

def minify_html_files(directory):
    for root, dirs, files in os.walk(directory):
        for file in files:
            if file.endswith('.html'):
                file_path = os.path.join(root, file)
                print(f"Minifying {file_path}")
                
                with open(file_path, 'r', encoding='utf-8') as f:
                    content = f.read()
                
                minified = htmlmin.minify(content,
                    remove_empty_space=True,
                    remove_all_empty_space=True,
                    remove_comments=True,
                    remove_optional_attribute_quotes=True
                )
                
                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(minified)

if __name__ == "__main__":
    html_dir = "data"
    minify_html_files(html_dir)
