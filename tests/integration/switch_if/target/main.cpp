struct Ped { int a, b, c; int state; };
void g(int v);

void OnCommand(Ped* p, int notify)
{
    switch (notify)
    {
        case 1:
            g(p->a);
            break;
    }
}
